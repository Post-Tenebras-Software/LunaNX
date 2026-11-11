#pragma once
#include "native_auth.hpp"

namespace luna::auth
{
class ProfileManager
{
public:
    enum class State { Closed, LoadingPicker, Picker, LoggingIn, LoadingForm, GeneratingName, Editing, Saving, Verifying, Saved, Failed };
    enum class Issue { None, Http, Transport, Response, Expired, Unsupported, Protected, InvalidName, Limit, NameRejected, Uncertain };
    enum Block : unsigned { MissingRoute=1, Pin=2, Household=4, Hidden=8, Migration=16, ProfileType=32, CreationLock=64 };
    enum class ParseStage { None, Json, Page, Widgets, Consent, Form, AvatarIdentity, AvatarLabel,
        SaveAction, ProfileIdentity, ProfileType, CurrentAvatar, Name, Gamertag, Complete, CreateOptions };
    struct Avatar { std::string id, label, artwork; };
    struct Entry { std::string id, name, artwork; bool editable = false; unsigned blockedBy = 0; };
    struct Diagnostics
    {
        struct Shape { std::uint64_t widget=0, action=0; };
        std::size_t tiles=0, editRoutes=0, createRoutes=0, addRoutes=0, legacyRoutes=0, sourcePins=0;
        ParseStage stage=ParseStage::None;
        std::size_t formWidgets=0, forms=0, avatars=0, saves=0;
        std::vector<Shape> shape;
    };
    struct Form
    {
        bool creating = false;
        std::string name, gamertag;
        std::vector<Avatar> avatars;
        std::size_t selectedAvatar = 0;
    };
    struct Status // Fixed categories and numbers only: safe for nxlink.
    {
        State state = State::Closed;
        Issue issue = Issue::None;
        long http = 0;
        int api = -1;
        bool writeAttempted = false;
        unsigned blockedBy = 0;
    };
    explicit ProfileManager(NativeAuthProbe& auth) : auth_(auth) {}
    ~ProfileManager();
    bool begin(std::int64_t nowMs);
    bool edit(std::size_t index, std::int64_t nowMs);
    bool create(std::int64_t nowMs);
    bool setName(const std::string& name);
    void selectAvatar(std::size_t index);
    bool canSave() const;
    bool save(std::int64_t nowMs); // One explicit confirmation, never an automatic retry.
    bool refresh(std::int64_t nowMs);
    bool back(std::int64_t nowMs);
    void close();
    std::optional<Request> nextRequest(std::int64_t nowMs);
    void complete(std::uint64_t id, long http, const std::string& body, std::int64_t nowMs);
    void transportFailed(std::uint64_t id);
    const Status& status() const { return status_; }
    const Form& form() const { return form_; }
    const std::vector<Entry>& profiles() const { return entries_; }
    const Diagnostics& diagnostics() const { return diagnostics_; }
    bool canCreate() const { return !createToken_.empty() && !creationLocked_; }
    bool active() const { return status_.state != State::Closed; }
    bool busy() const;
    static const char* message(const Status& status);
    static const char* blockLabel(unsigned blockedBy);
    static const char* parseLabel(ParseStage stage);
    static bool validName(const std::string& name);
private:
    enum class Operation { Picker, Login, Form, Generate, Write, VerifyPicker, VerifyForm, CreateOptions };
    void fail(Issue issue);
    void clearForm();
    void clearRoutes();
    bool alive(std::int64_t nowMs);
    void parsePicker(const std::string& body);
    bool parseForm(const std::string& body);
    NativeAuthProbe& auth_;
    Status status_;
    Diagnostics diagnostics_;
    Form form_;
    std::vector<Entry> entries_;
    std::vector<std::string> editTokens_;
    std::string createToken_, formToken_, profileToken_, targetId_, originalName_, originalAvatar_, nameToken_;
    std::string desiredName_, desiredAvatar_;
    std::vector<std::string> beforeIds_;
    bool creationLocked_ = false, verifying_ = false, createNeedsOptions_ = false;
    unsigned formPageHops_=0;
    std::int64_t deadline_ = 0;
    Operation operation_ = Operation::Picker;
    std::optional<std::uint64_t> pending_;
    std::uint64_t nextId_ = (std::uint64_t{1} << 60);
};
}
