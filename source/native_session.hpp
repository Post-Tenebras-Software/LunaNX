#pragma once

#include "native_auth.hpp"
#include "luna_session.hpp"
#include "video_mode.hpp"
#include <memory>

namespace luna::auth
{
// One native, normal-solo attempt. Credentials remain in NativeAuthProbe;
// the transport receives only bounded requests to fixed authenticated routes.
class NativeSession
{
public:
    enum class State { Idle, CheckingPin, AwaitOffer, Provisioning, Ready, Blocked, Failed, Cancelled,
        SoftwarePage, SoftwareNotice, EulaPage, EulaNotice, EulaAccept, Ending, Ended, EndFailed };
    enum class Block { None, ParentalPin, ParentalDenied, Terms, AgeRating, Eula,
        SoftwareDecode, MouseKeyboard, UnsupportedLaunch, InvalidResponse, Expired, Transport, Http };
    enum class Requirement { None, Transfer, RevokeActivity, Party, StreamerMode,
        Build, BuildConfiguration, SaveSlot, ParentalProof };
    enum class ValueKind { None, Boolean, String, ZeroNumber, Number, Array, Object };
    enum class Quality { MatchMode, Standard };
    enum class Operation { None, Pin, Refresh, Create, Poll, Cancel, SoftwarePage, EulaPage, EulaAccept, End };
    enum class HttpError { None, Unknown, InvalidRequest, Validation, InvalidParameter,
        MissingParameter, Unauthorized, AccessDenied, InvalidToken, Throttled, Internal };
    enum class ErrorBody { None, Empty, TooLarge, Unparseable, Object };
    enum class ErrorField { None, Timeout, GameId, PlayToken, SignalingRequest, ClientContext,
        ClientFeatures, DataUsage, Routing, RegionLatencies, RegionPreferences, ProductStage,
        Tdi, ParentalToken, SaveSlot, JobId, Transfer, RevokeActivity, SourceToken };
    struct HttpDiagnostic
    {
        Operation operation = Operation::None;
        HttpError error = HttpError::None;
        ErrorBody body = ErrorBody::None;
        ErrorField fieldHint = ErrorField::None;
        std::size_t responseBytes = 0;
        std::uint32_t fieldHints = 0;
    };
    struct Status
    {
        State state = State::Idle;
        Block block = Block::None;
        Requirement requirement = Requirement::None;
        ValueKind requirementKind = ValueKind::None;
        unsigned unsupportedFields = 0;
        long httpStatus = 0;
        std::int64_t apiErrorCode = 0;
        bool refreshing = false, remoteWorkMayRemain = false;
        session::State jobState = session::State::Idle;
        session::Error jobError = session::Error::None;
        std::optional<std::int64_t> position, waitSeconds;
        NoticeDiagnostics softwareNotice;
        bool eulaAccepted = false;
        HttpDiagnostic httpDiagnostic;
    };
    explicit NativeSession(NativeAuthProbe& auth);
    ~NativeSession();
    bool start(std::int64_t now, bool hardwareDecoderPrepared = false);
    bool supplyOffer(std::string sdp, std::int64_t now, video::Mode mode = video::Mode::P720_30,
        Quality quality = Quality::MatchMode);
    bool chooseSoftwareNoticeButton(std::size_t index, std::int64_t now);
    bool chooseEulaButton(std::size_t index, std::int64_t now);
    std::optional<Request> nextRequest(std::int64_t now);
    void complete(std::uint64_t id, long http, const std::string& body, std::int64_t now);
    void transportFailed(std::uint64_t id, std::int64_t now);
    void cancel(std::int64_t now);
    bool canEnd() const;
    bool endGame(std::int64_t now); // Explicit confirmation only; cancel() never terminates a game.
    std::optional<std::string> takeAnswer();
    const Status& status() const { return status_; }
    const ExperimentalNotice& softwareNotice() const { return softwareNotice_; }
    static const char* blockLabel(Block block);
    static const char* requirementLabel(Requirement requirement);
    static const char* valueKindLabel(ValueKind kind);
    static const char* operationLabel(Operation operation);
    static const char* httpErrorLabel(HttpError error);
    static const char* errorBodyLabel(ErrorBody body);
    static const char* errorFieldLabel(ErrorField field);
private:
    struct Pending { std::uint64_t id, ownerId; Operation operation; };
    Request issue(Operation operation, std::string body, std::uint64_t ownerId = 0);
    void fail(Block block);
    void sync();
    void clearSoftwareNotice();
    void clearNoticeContent();
    NativeAuthProbe& auth_;
    Status status_;
    std::string payload_, ratingAction_, softwareToken_, eulaToken_, eulaProduct_;
    std::string allocatedSession_, launchProfile_;
    ExperimentalNotice softwareNotice_;
    std::vector<std::string> softwareActions_;
    std::optional<Pending> pending_;
    std::unique_ptr<session::ProvisioningSession> owner_;
    std::int64_t now_ = 0, deadline_ = 0;
    std::uint64_t nextId_ = 10'000'000;
};
}
