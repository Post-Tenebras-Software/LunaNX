#include "native_profiles.hpp"
#include "launcher_media.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace luna::auth
{
namespace
{
using Json = nlohmann::json;
constexpr const char* Origin = "https://proxy-prod.us-east-1.tempo.digital.a2z.com";
void wipe(std::string& s) { volatile char* p = s.empty() ? nullptr : &s[0]; for (std::size_t i=0;i<s.size();++i) p[i]=0; s.clear(); }
Json parse(const std::string& body)
{
    if (body.empty() || body.size() > MaxLunaResponse) throw std::invalid_argument("Profile response size");
    std::vector<std::set<std::string>> keys;
    auto value = Json::parse(body, [&](int depth, Json::parse_event_t event, Json& item)
    {
        if (depth > 32) throw std::invalid_argument("Profile response depth");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second)
            throw std::invalid_argument("Duplicate profile field");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
    if (!value.is_object()) throw std::invalid_argument("Profile object required");
    return value;
}
std::string ascii(const Json& value, const char* key, std::size_t max = 512)
{
    const auto s = value.at(key).get<std::string>();
    if (s.empty() || s.size() > max || std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
        throw std::invalid_argument("Profile identifier");
    return s;
}
std::string display(const Json& value, const char* key, std::size_t max = 256)
{
    const auto s = value.at(key).get<std::string>();
    if (s.empty() || s.size() > max || std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
        throw std::invalid_argument("Profile display text");
    return s;
}
std::string image(const Json& value, const char* key)
{
    if (!value.contains(key) || !value[key].is_string()) return {};
    const auto url = value[key].get<std::string>();
    return launcher::artworkUrl(url) ? url : std::string();
}
Json presentation(const Json& widget)
{
    const auto& raw = widget.at("presentationData");
    const auto data = raw.is_string() ? parse(raw.get<std::string>()) : raw;
    if (!data.is_object()) throw std::invalid_argument("Profile presentation");
    return data;
}
bool flag(const Json& data, const char* key) { return data.contains(key) && !data[key].is_null() && data.at(key).get<bool>(); }
std::uint64_t shapeFingerprint(const std::string& name)
{
    if (name.empty() || name.size()>80 || name.front()<'A' || name.front()>'Z' ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos) return 0;
    std::uint64_t value=14695981039346656037ull;
    for (unsigned char c:name) { value^=c; value*=1099511628211ull; }
    return value;
}
template<class F> void widgets(const Json& page, F visit)
{
    if (page.contains("error") && !page["error"].is_null()) throw std::invalid_argument("Profile page error");
    if (page.contains("actions") && !page["actions"].is_null() && !page["actions"].empty())
        throw std::invalid_argument("Profile page requires another action");
    const auto& groups = page.at("pageMemberGroups");
    if (!groups.is_object() || groups.empty()) throw std::invalid_argument("Profile groups");
    std::size_t count=0;
    const auto walk = [&](const auto& self, const Json& list, unsigned depth) -> void
    {
        if (!list.is_array() || depth > 24) throw std::invalid_argument("Profile widgets");
        for (const auto& widget : list)
        {
            if (!widget.is_object() || ++count > 4096) throw std::invalid_argument("Profile widget bound");
            visit(widget);
            if (widget.contains("widgets") && !widget["widgets"].is_null()) self(self, widget["widgets"], depth+1);
        }
    };
    for (const auto& group : groups.items())
        if (group.value().contains("widgets") && !group.value()["widgets"].is_null()) walk(walk, group.value()["widgets"], 0);
}
Json actions(const Json& widget)
{
    const auto result = widget.value("actions", Json::array());
    if (!result.is_array() || result.size() > 16) throw std::invalid_argument("Profile actions");
    return result;
}
std::string trim(std::string s)
{
    const auto first=s.find_first_not_of(' ');
    if (first==std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(' ')-first+1);
}
}
ProfileManager::~ProfileManager() { clearForm(); clearRoutes(); }
bool ProfileManager::validName(const std::string& name)
{
    if (name.empty() || name.size() > 200 || name != trim(name) || name.find_first_of("<>") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; })) return false;
    try { (void)Json(name).dump(); } catch (...) { return false; }
    std::size_t units = 0;
    for (unsigned char c : name) if ((c & 0xc0) != 0x80) units += c >= 0xf0 ? 2 : 1;
    return units <= 50; // Same UTF-16 character bound as PlayerDetailsForm (11423).
}
bool ProfileManager::busy() const
{
    const auto s = status_.state;
    return s == State::LoadingPicker || s == State::LoggingIn || s == State::LoadingForm ||
        s == State::GeneratingName || s == State::Saving || s == State::Verifying;
}
void ProfileManager::clearRoutes()
{
    wipe(createToken_); for (auto& t : editTokens_) wipe(t); editTokens_.clear(); entries_.clear(); creationLocked_=false; createNeedsOptions_=false;
}
void ProfileManager::clearForm()
{
    wipe(formToken_); wipe(profileToken_); wipe(nameToken_); wipe(targetId_);
    wipe(desiredName_); wipe(desiredAvatar_); form_={}; originalName_.clear(); originalAvatar_.clear(); verifying_=false;
    formPageHops_=0; diagnostics_.stage=ParseStage::None; diagnostics_.shape.clear();
    diagnostics_.formWidgets=diagnostics_.forms=diagnostics_.avatars=diagnostics_.saves=0;
}
void ProfileManager::fail(Issue issue)
{
    status_.issue = verifying_ && issue != Issue::Protected && issue != Issue::Expired ? Issue::Uncertain : issue;
    status_.state = State::Failed; pending_.reset(); wipe(profileToken_); wipe(nameToken_);
}
bool ProfileManager::alive(std::int64_t now)
{
    if (auth_.status_.step != Step::ChooseProfile || now >= auth_.sessionDeadline_ || auth_.accessToken_.empty())
    { fail(Issue::Expired); return false; }
    return true;
}
bool ProfileManager::begin(std::int64_t now)
{
    if (active() || auth_.status_.step != Step::ChooseProfile || auth_.pending_) return false;
    status_={}; if (!alive(now)) return false;
    status_.state=State::LoadingPicker; operation_=Operation::Picker; deadline_=std::min(auth_.sessionDeadline_, now+60'000); return true;
}
bool ProfileManager::refresh(std::int64_t now)
{
    if (busy() || !active() || !alive(now)) return false;
    clearForm(); status_={}; status_.state=State::LoadingPicker; operation_=Operation::Picker;
    deadline_=std::min(auth_.sessionDeadline_, now+60'000); return true;
}
bool ProfileManager::edit(std::size_t index, std::int64_t now)
{
    if (status_.state!=State::Picker || index>=entries_.size() || !alive(now)) return false;
    if (!entries_[index].editable)
    {
        status_.blockedBy=entries_[index].blockedBy;
        status_.issue=(status_.blockedBy & ~MissingRoute) ? Issue::Protected : Issue::Unsupported;
        return false;
    }
    clearForm(); status_={}; targetId_=entries_[index].id; formToken_=editTokens_.at(index);
    status_.state=State::LoggingIn; operation_=Operation::Login; deadline_=std::min(auth_.sessionDeadline_,now+60'000); return true;
}
bool ProfileManager::create(std::int64_t now)
{
    if (status_.state!=State::Picker || !alive(now)) return false;
    if (!canCreate())
    {
        status_.blockedBy=creationLocked_?CreationLock:MissingRoute;
        status_.issue=creationLocked_?Issue::Protected:Issue::Unsupported;
        return false;
    }
    clearForm(); status_={}; form_.creating=true; formToken_=createToken_;
    status_.state=State::LoadingForm; operation_=createNeedsOptions_?Operation::CreateOptions:Operation::Form;
    deadline_=std::min(auth_.sessionDeadline_,now+60'000); return true;
}
bool ProfileManager::setName(const std::string& name)
{
    if (status_.state!=State::Editing) return false;
    const auto value=trim(name);
    if (!validName(value)) { status_.issue=Issue::InvalidName; return false; }
    form_.name=value; status_.issue=Issue::None; return true;
}
void ProfileManager::selectAvatar(std::size_t index)
{ if (status_.state==State::Editing && index<form_.avatars.size()) { form_.selectedAvatar=index; status_.issue=Issue::None; } }
bool ProfileManager::canSave() const
{
    return status_.state==State::Editing && validName(form_.name) && form_.selectedAvatar<form_.avatars.size() &&
        (form_.creating ? !nameToken_.empty() && !form_.gamertag.empty() :
            form_.name!=originalName_ || form_.avatars[form_.selectedAvatar].id!=originalAvatar_);
}
bool ProfileManager::save(std::int64_t now)
{
    if (!canSave() || !alive(now)) return false;
    desiredName_=form_.name; desiredAvatar_=form_.avatars[form_.selectedAvatar].id;
    beforeIds_.clear(); for (const auto& entry:entries_) beforeIds_.push_back(entry.id);
    status_.issue=Issue::None; status_.state=State::Saving; operation_=Operation::Write;
    deadline_=std::min(auth_.sessionDeadline_,now+60'000); return true;
}
bool ProfileManager::back(std::int64_t now)
{
    if (status_.state==State::Saving) return false; // A POST in flight cannot safely be cancelled/retried.
    pending_.reset();
    if (status_.state==State::Picker || (status_.state==State::LoadingPicker && operation_==Operation::Picker)) { close(); return true; }
    if (!active()) return false;
    clearForm(); status_={}; status_.state=State::LoadingPicker; operation_=Operation::Picker;
    if (!alive(now)) { close(); return true; }
    deadline_=std::min(auth_.sessionDeadline_,now+60'000); return true;
}
void ProfileManager::close()
{
    if (status_.state==State::Saving) return;
    pending_.reset(); clearForm(); clearRoutes(); status_={};
    if (auth_.status_.step==Step::ChooseProfile)
    { auth_.status_.step=Step::ProfilesPage; auth_.deadline_=auth_.sessionDeadline_; }
}
std::optional<Request> ProfileManager::nextRequest(std::int64_t now)
{
    if (!active() || !alive(now)) return std::nullopt;
    if (busy() && now>=deadline_) { fail(status_.writeAttempted ? Issue::Uncertain : Issue::Transport); return std::nullopt; }
    if (!busy() || pending_) return std::nullopt;
    Request req{nextId_++,Step::ProfileManagement,std::string(Origin),{}, {"Content-Type: text/plain;charset=UTF-8","Accept-Language: en_US"}};
    Json body;
    switch (operation_)
    {
    case Operation::Picker: case Operation::VerifyPicker:
        req.url+="/getPage"; body={{"featureScheme","WEB_V1"},
            {"pageContext",{{"pageType",auth_.profilePageType_},{"pageId",auth_.profilePageId_}}}}; break;
    case Operation::Form: case Operation::VerifyForm: case Operation::CreateOptions:
        req.url+="/getPage"; body={{"featureScheme","WEB_V1"},{"serviceToken",formToken_}}; break;
    case Operation::Login: req.url+="/player/playerLogin"; body={{"profileId",targetId_}}; break;
    case Operation::Generate: req.url+="/player/generatePlayerName"; body=Json::object(); break;
    case Operation::Write:
        req.url+=form_.creating ? "/player/createPlayer" : "/player/setPlayer";
        body={{"avatarId",desiredAvatar_},{"displayName",form_.gamertag},{"profileName",desiredName_}};
        if (form_.creating) { body["nameToken"]=nameToken_; body["profileType"]="ADULT"; }
        status_.writeAttempted=true; break;
    }
    if (req.url==std::string(Origin)+"/getPage")
    {
        body["clientContext"]=Json::parse(auth_.proxyContext()); body["dynamicFeatures"]=Json::array();
        body["inputContext"]={{"gamepadTypes",Json::array()}};
    }
    auth_.proxyHeaders(req,false);
    // Login and creation use account authentication; form/edit uses only the
    // proof obtained by logging into this target profile, never a previous one.
    if (!profileToken_.empty() && (operation_==Operation::Form || operation_==Operation::VerifyForm || operation_==Operation::Write))
        req.headers.push_back("x-amz-profile-token: "+profileToken_);
    req.body=body.dump(); pending_=req.id; return req;
}
void ProfileManager::parsePicker(const std::string& body)
{
    const auto page=parse(body);
    diagnostics_={};
    std::vector<Entry> entries; std::vector<std::string> tokens; std::string create, add;
    bool locked=false; std::set<std::string> seen;
    widgets(page,[&](const Json& w)
    {
        const auto type=w.value("type","");
        if (type=="PLAYER_SELECTION_COLLECTION") locked |= flag(presentation(w),"creationAndRemovalLockEnabled");
        if (type!="PLAYER_SELECTION_TILE") return;
        ++diagnostics_.tiles;
        const auto data=presentation(w); const auto list=actions(w);
        std::string edit;
        for (const auto& action:list)
        {
            const auto target=action.value("target","");
            if (target=="PLAYER_CREATE")
            {
                ++diagnostics_.createRoutes;
                const auto token=ascii(action,"serviceToken",32768);
                if (!create.empty() && create!=token) throw std::invalid_argument("Multiple create routes");
                create=token;
            }
            if (target=="PLAYER_DETAILS_EDIT" || target=="PLAYER_EDIT")
            {
                ++diagnostics_.editRoutes;
                const auto token=ascii(action,"serviceToken",32768);
                if (!edit.empty() && edit!=token) throw std::invalid_argument("Multiple edit routes");
                edit=token;
            }
            if (target=="PLAYER_ADD")
            {
                ++diagnostics_.addRoutes;
                const auto token=ascii(action,"serviceToken",32768);
                if (!add.empty() && add!=token) throw std::invalid_argument("Multiple add routes");
                add=token;
            }
            if (target=="PLAYER_LANDING" || target=="PROFILE_SELECT") ++diagnostics_.legacyRoutes;
        }
        std::string id, profileType;
        if (data.contains("profileType") && !data["profileType"].is_null()) profileType=ascii(data,"profileType",32);
        if (data.contains("profileId") && !data["profileId"].is_null() && data["profileId"]!="") id=ascii(data,"profileId");
        for (const auto& a:list) if (a.value("target","")=="PLAYER_SELECT" || a.value("target","")=="PLAYER_EDIT")
        {
            const auto& raw=a.at("payload"); const auto p=raw.is_string()?parse(raw.get<std::string>()):raw;
            const auto selectedId=ascii(p,"profileId");
            if (!id.empty() && selectedId!=id) throw std::invalid_argument("Profile identity mismatch");
            id=selectedId;
            if (p.contains("profileType") && !p["profileType"].is_null())
            {
                const auto selectedType=ascii(p,"profileType",32);
                if (!profileType.empty() && profileType!=selectedType) throw std::invalid_argument("Profile type mismatch");
                profileType=selectedType;
            }
        }
        if (id.empty()) return;
        if (entries.size()>=16 || !seen.insert(id).second) throw std::invalid_argument("Profile count/identity");
        if (flag(data,"sourcePinExists")) ++diagnostics_.sourcePins;
        // PlayerSelectionTile (44838) shows the source-PIN setup prompt only
        // during access, not edit. isLocked is the actual edit PIN gate.
        unsigned blocked=edit.empty()?MissingRoute:0u;
        if (!profileType.empty() && profileType!="ADULT") blocked|=ProfileType;
        if (flag(data,"isLocked")) blocked|=Pin;
        if (flag(data,"isHouseholdMismatch")) blocked|=Household;
        if (flag(data,"hideInEditMode")) blocked|=Hidden;
        if (data.contains("migrationStatus") && !data["migrationStatus"].is_null() && data["migrationStatus"]!=0 && data["migrationStatus"]!=false && data["migrationStatus"]!="") blocked|=Migration;
        entries.push_back({id,display(data,"primaryText"),image(data,"iconUrl"),blocked==0,blocked}); tokens.push_back(std::move(edit));
    });
    clearRoutes(); entries_=std::move(entries); editTokens_=std::move(tokens);
    createNeedsOptions_=create.empty()&&!add.empty(); createToken_=createNeedsOptions_?std::move(add):std::move(create); creationLocked_=locked;
}
bool ProfileManager::parseForm(const std::string& body)
{
    diagnostics_.stage=ParseStage::Json; diagnostics_.formWidgets=diagnostics_.forms=diagnostics_.avatars=diagnostics_.saves=0;
    diagnostics_.shape.clear();
    auto page=parse(body); std::optional<Json> data; std::vector<Avatar> avatars; bool save=false;
    unsigned editPanels=0, typeGuides=0; bool adultOffered=false;
    // PlayerDetailsForm (11423) renders this footer only for non-edit CHILD /
    // TEEN creation. This manager creates ADULT profiles only; scanning the
    // dormant footer must not turn its hidden controls into required consent.
    if (page.contains("pageMemberGroups") && page["pageMemberGroups"].is_object())
        page["pageMemberGroups"].erase("profilePrivacyFooter");
    std::set<std::string> ids;
    diagnostics_.stage=ParseStage::Page;
    widgets(page,[&](const Json& w)
    {
        diagnostics_.stage=ParseStage::Widgets; ++diagnostics_.formWidgets;
        const auto type=w.value("type","");
        const auto recordShape=[&](std::uint64_t action)
        {
            const auto widget=shapeFingerprint(type);
            if (widget && diagnostics_.shape.size()<64 && std::none_of(diagnostics_.shape.begin(),diagnostics_.shape.end(),
                [&](const auto& s){return s.widget==widget&&s.action==action;}))
                diagnostics_.shape.push_back({widget,action});
        };
        recordShape(0);
        const auto offered=actions(w);
        for (const auto& action:offered)
            recordShape(shapeFingerprint(action.value("target","")));
        if (type.find("CHECKBOX")!=std::string::npos || type.find("CONSENT")!=std::string::npos || type=="LEGAL_TERMS")
        { diagnostics_.stage=ParseStage::Consent; throw std::invalid_argument("Profile requires another consent flow"); }
        if (type=="PLAYER_DETAILS_FORM")
        { diagnostics_.stage=ParseStage::Form; ++diagnostics_.forms; if (data) throw std::invalid_argument("Multiple profile forms"); data=presentation(w); }
        if (type=="EDIT_PROFILE_PANEL") ++editPanels;
        if (type=="PROFILE_TYPE_GUIDANCE")
        {
            ++typeGuides; const auto guide=presentation(w);
            if (guide.contains("options") && guide["options"].is_array())
                for (const auto& option:guide["options"]) if (option.value("type","")=="ADULT") adultOffered=true;
        }
        if (type=="PLAYER_AVATAR_TILE")
        {
            diagnostics_.stage=ParseStage::AvatarIdentity; ++diagnostics_.avatars;
            const auto d=presentation(w);
            std::string id;
            if (w.contains("id") && !w["id"].is_null())
            {
                const auto widgetId=ascii(w,"id",1024);
                // AvatarCollection (12358) sends widget.id.substring(19).
                if (widgetId.size()<=19 || widgetId.size()>19+512) throw std::invalid_argument("Avatar widget identity");
                id=widgetId.substr(19);
            }
            if (d.contains("avatarId") && !d["avatarId"].is_null())
            {
                const auto presented=ascii(d,"avatarId");
                if (!id.empty() && id!=presented) throw std::invalid_argument("Avatar identity mismatch");
                id=presented;
            }
            if (id.empty()) throw std::invalid_argument("Avatar identity missing");
            if (avatars.size()>=256 || !ids.insert(id).second) throw std::invalid_argument("Avatar bound/identity");
            diagnostics_.stage=ParseStage::AvatarLabel;
            avatars.push_back({id,d.contains("displayName") && !d["displayName"].is_null() && d["displayName"]!=""?
                display(d,"displayName"):"Luna avatar",image(d,"avatarUrl")});
        }
        diagnostics_.stage=ParseStage::SaveAction;
        for (const auto& action:actions(w)) if (action.value("target","")=="PLAYER_DETAILS_SAVE") { save=true; ++diagnostics_.saves; }
    });
    diagnostics_.stage=ParseStage::Form;
    if (!data && avatars.empty() && !save && ((editPanels==1 && (!form_.creating || verifying_)) ||
        (form_.creating && !verifying_ && typeGuides==1 && adultOffered)))
    {
        // EditProfilePanel (36969) renders this separate button group;
        // PLAYER_DETAILS_EDIT then opens the actual name/avatar form.
        // Creation can first render PROFILE_TYPE_GUIDANCE (99275).
        const auto& groups=page.at("pageMemberGroups");
        const auto groupName="editProfileChangeButton";
        std::string token;
        const auto accept=[&](const Json& w)
        {
            for (const auto& action:actions(w))
                if (action.value("target","")== (editPanels==1?"PLAYER_DETAILS_EDIT":"PLAYER_CREATE"))
                {
                    const auto offered=ascii(action,"serviceToken",32768);
                    if (!token.empty() && token!=offered) throw std::invalid_argument("Ambiguous form route");
                    token=offered;
                }
        };
        if (editPanels==1)
        {
            if (!groups.contains(groupName)) throw std::invalid_argument("Missing edit panel button group");
            widgets(Json{{"pageMemberGroups",{{groupName,groups.at(groupName)}}}},accept);
        }
        else widgets(page,accept);
        if (token.empty() || token==formToken_ || ++formPageHops_>3) throw std::invalid_argument("Invalid form followup");
        wipe(formToken_); formToken_=std::move(token);
        return false;
    }
    if (!data || !save || avatars.empty()) throw std::invalid_argument("Incomplete profile form");
    diagnostics_.stage=ParseStage::ProfileIdentity;
    if ((!form_.creating || verifying_) && data->contains("profileId") && !(*data)["profileId"].is_null() && ascii(*data,"profileId")!=targetId_)
        throw std::invalid_argument("Profile form identity mismatch");
    diagnostics_.stage=ParseStage::ProfileType;
    if (data->contains("profileType") && !(*data)["profileType"].is_null() && ascii(*data,"profileType",32)!="ADULT")
        throw std::invalid_argument("Unsupported profile type");
    diagnostics_.stage=ParseStage::CurrentAvatar;
    const auto avatar=ascii(*data,"avatarId");
    const auto selected=std::find_if(avatars.begin(),avatars.end(),[&](const auto& a){return a.id==avatar;});
    if (selected==avatars.end()) throw std::invalid_argument("Current avatar missing");
    diagnostics_.stage=ParseStage::Name;
    const auto name=(form_.creating && !verifying_) || !data->contains("profileName") || (*data)["profileName"].is_null() || (*data)["profileName"]=="" ?
        std::string() : display(*data,"profileName",200);
    if (!name.empty() && !validName(name)) throw std::invalid_argument("Invalid profile name");
    diagnostics_.stage=ParseStage::Gamertag;
    form_.name=name; form_.gamertag=form_.creating && !verifying_ ? std::string() : display(*data,"displayName");
    form_.selectedAvatar=static_cast<std::size_t>(selected-avatars.begin()); form_.avatars=std::move(avatars);
    originalName_=name; originalAvatar_=avatar;
    diagnostics_.stage=ParseStage::Complete;
    return true;
}
void ProfileManager::complete(std::uint64_t id,long http,const std::string& body,std::int64_t now)
{
    if (!pending_ || *pending_!=id || !active()) return;
    pending_.reset(); status_.http=http;
    if (!alive(now)) return;
    if (now>=deadline_) { fail(status_.writeAttempted?Issue::Uncertain:Issue::Transport); return; }
    if (http==401 || http==403) { auth_.fail(Error::Http,http); fail(Issue::Protected); return; }
    if (http!=200) { fail(operation_==Operation::Write?Issue::Uncertain:Issue::Http); return; }
    try
    {
        const auto value=parse(body);
        if (value.contains("error") && !value["error"].is_null())
        { fail(operation_==Operation::Write?Issue::Uncertain:Issue::Response); return; }
        if (value.contains("errorCode") && !value["errorCode"].is_null() && value["errorCode"]!=0)
        {
            status_.api=value["errorCode"].is_number_integer()?value["errorCode"].get<int>():-1;
            fail(status_.api==28 || status_.api==29 || status_.api==30 ? Issue::Protected : status_.api==20 ? Issue::Limit :
                status_.api>=11 && status_.api<=25 ? Issue::NameRejected : Issue::Response); return;
        }
        switch (operation_)
        {
        case Operation::Picker: parsePicker(body); status_.state=State::Picker; break;
        case Operation::CreateOptions:
        {
            diagnostics_.stage=ParseStage::CreateOptions;
            std::string token;
            widgets(value,[&](const Json& w)
            {
                for (const auto& action:actions(w)) if (action.value("target","")=="PLAYER_CREATE")
                {
                    const auto offered=ascii(action,"serviceToken",32768);
                    if (!token.empty() && token!=offered) throw std::invalid_argument("Ambiguous profile creation");
                    token=offered;
                }
            });
            if (token.empty()) { fail(Issue::Unsupported); break; }
            wipe(formToken_); formToken_=std::move(token); operation_=Operation::Form; status_.state=State::LoadingForm;
            break;
        }
        case Operation::Login:
            if (value.contains("profileId") && ascii(value,"profileId")!=targetId_) throw std::invalid_argument("Login identity mismatch");
            wipe(profileToken_); profileToken_=ascii(value,"token",32768);
            status_.state=verifying_?State::Verifying:State::LoadingForm;
            operation_=verifying_?Operation::VerifyForm:Operation::Form; break;
        case Operation::Form: case Operation::VerifyForm:
            if (!parseForm(body)) break;
            if (verifying_)
            {
                if (form_.name!=desiredName_ || form_.avatars[form_.selectedAvatar].id!=desiredAvatar_)
                { fail(Issue::Uncertain); return; }
                status_.state=State::Saved; status_.issue=Issue::None; wipe(profileToken_); wipe(nameToken_);
            }
            else if (form_.creating) { status_.state=State::GeneratingName; operation_=Operation::Generate; }
            else status_.state=State::Editing;
            break;
        case Operation::Generate:
            form_.gamertag=display(value,"displayName"); nameToken_=ascii(value,"nameToken",32768); status_.state=State::Editing; break;
        case Operation::Write:
            verifying_=true; wipe(nameToken_); status_.state=State::Verifying; operation_=Operation::VerifyPicker; break;
        case Operation::VerifyPicker:
        {
            parsePicker(body); std::vector<std::size_t> matches;
            for (std::size_t i=0;i<entries_.size();++i)
                if (form_.creating ? entries_[i].name==desiredName_ && std::find(beforeIds_.begin(),beforeIds_.end(),entries_[i].id)==beforeIds_.end() : entries_[i].id==targetId_)
                    matches.push_back(i);
            if (matches.size()!=1 || !entries_[matches[0]].editable) { fail(Issue::Uncertain); return; }
            targetId_=entries_[matches[0]].id; formToken_=editTokens_[matches[0]];
            formPageHops_=0;
            operation_=Operation::Login; status_.state=State::Verifying; break;
        }
        }
        if (!busy()) deadline_=auth_.sessionDeadline_;
    }
    catch (...) { fail(operation_==Operation::Write?Issue::Uncertain:Issue::Response); }
}
void ProfileManager::transportFailed(std::uint64_t id)
{
    if (!pending_ || *pending_!=id) return;
    fail(status_.writeAttempted?Issue::Uncertain:Issue::Transport);
}
const char* ProfileManager::message(const Status& s)
{
    if (s.blockedBy) return blockLabel(s.blockedBy);
    switch (s.issue)
    {
    case Issue::Protected: return "This profile or action needs setup or verification this version doesn't support. Manage it in Luna.";
    case Issue::InvalidName: return "Use 1-50 characters for the profile name, without < or >.";
    case Issue::Limit: return "Amazon says this account has reached its profile limit.";
    case Issue::NameRejected: return "Amazon rejected the name or name token. Return to Manage Profiles and try again.";
    case Issue::Uncertain: return "The change may have been saved, but couldn't be verified. Reopen Manage Profiles to check before trying again.";
    case Issue::Expired: return "Your sign-in expired. Return to sign-in and try again.";
    case Issue::Http: case Issue::Transport: return "Couldn't load profile settings. Check your connection, then return and try again.";
    case Issue::Response: case Issue::Unsupported: return "This profile setup isn't supported yet. You can manage it in Luna.";
    case Issue::None: break;
    }
    if (s.state==State::Saved) return "Saved to your Amazon account and verified.";
    if (s.state==State::Verifying) return "Checking the saved profile and avatar with Amazon.";
    if (s.state==State::Saving) return "Saving to Amazon. Please wait.";
    return "";
}
const char* ProfileManager::blockLabel(unsigned blocked)
{
    if (blocked & Pin) return "PIN verification required in Luna";
    if (blocked & Household) return "Household verification required in Luna";
    if (blocked & Migration) return "Profile migration required in Luna";
    if (blocked & ProfileType) return "Child/teen profile editing is not supported yet";
    if (blocked & Hidden) return "Amazon hides this profile from editing";
    if (blocked & CreationLock) return "Profile creation is locked in Luna";
    if (blocked & MissingRoute) return "Luna did not provide a supported edit/create route";
    return "Edit name and avatar";
}
const char* ProfileManager::parseLabel(ParseStage stage)
{
    switch (stage)
    {
    case ParseStage::None:return "none"; case ParseStage::Json:return "json";
    case ParseStage::Page:return "page"; case ParseStage::Widgets:return "widgets";
    case ParseStage::Consent:return "consent"; case ParseStage::Form:return "form";
    case ParseStage::AvatarIdentity:return "avatar_identity"; case ParseStage::AvatarLabel:return "avatar_label";
    case ParseStage::SaveAction:return "save_action"; case ParseStage::ProfileIdentity:return "profile_identity";
    case ParseStage::ProfileType:return "profile_type"; case ParseStage::CurrentAvatar:return "current_avatar";
    case ParseStage::Name:return "name"; case ParseStage::Gamertag:return "gamertag";
    case ParseStage::Complete:return "complete"; case ParseStage::CreateOptions:return "create_options";
    }
    return "unknown";
}
}
