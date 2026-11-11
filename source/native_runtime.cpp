#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <rtc/rtc.hpp>
#include <exception>
#include <stdexcept>
#include <future>
#include <memory>
#include <chrono>
#include <atomic>
#include "h264_decoder.hpp"
#include "video_view.hpp"
#include "opus_audio.hpp"
#include "controller_input.hpp"
#include "local_shortcuts.hpp"
#include "local_control_session.hpp"
#include "native_runtime.hpp"
#include "frame_pacer.hpp"
#include "performance_overlay.hpp"

std::string getSwitchIpv4()
{
    Result ipResult = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(ipResult))
    {
        throw std::runtime_error("Network information service failed.");
    }

    u32 localIp = 0;
    ipResult = nifmGetCurrentIpAddress(&localIp);
    nifmExit();

    if (R_FAILED(ipResult) || localIp == 0)
    {
        throw std::runtime_error("IPv4 lookup failed.");
    }

    char bindAddress[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &localIp, bindAddress, sizeof(bindAddress)) == nullptr)
    {
        throw std::runtime_error("IPv4 address conversion failed.");
    }

    return std::string(bindAddress);
}

struct WebRtcSession
{
    bool live = false;
    std::shared_ptr<rtc::PeerConnection> peer;
    std::shared_ptr<rtc::Track> video;
    std::shared_ptr<rtc::Track> audio;
    std::shared_ptr<rtc::DataChannel> channel;
    std::shared_ptr<rtc::DataChannel> controlChannel;
    std::shared_ptr<luna::control::LocalControlSession> controlSession;
    std::uint64_t controlReportRevision = 0;
    std::shared_ptr<H264Decoder> decoder;
    std::shared_ptr<std::atomic<std::uint64_t>> encodedVideoBytes;
    std::shared_ptr<OpusAudioPlayer> audioPlayer;
    ControllerInputSender controllerInput;
    std::string offerSdp;
};

void stopWebRtc(WebRtcSession& session)
{
    session.controllerInput.stop(session.channel);
    if (session.controlSession) session.controlSession->stop();
    if (session.peer)
        session.peer->close();

    session.channel.reset();
    session.controlChannel.reset();
    session.audio.reset();
    session.video.reset();
    session.peer.reset();
    session.decoder.reset();
    session.encodedVideoBytes.reset();
    session.offerSdp.clear();
    session.controlReportRevision = 0;

    try
    {
        rtc::Cleanup().get();
        printf("WebRTC cleanup complete.\n");
    }
    catch (const std::exception& error)
    {
        printf("WebRTC cleanup failed: %s\n", error.what());
    }
    session.controlSession.reset();

    if (session.audioPlayer)
    {
        session.audioPlayer->stop();
        session.audioPlayer.reset();
    }
}

bool updateSessionControl(WebRtcSession& session)
{
    if (!session.controlSession || !session.controlChannel) return false;
    try
    {
        const auto& channel = session.controlChannel;
        session.controlSession->update(channel->isOpen(), channel->bufferedAmount() != 0,
            [&](const luna::control::Bytes& bytes)
            {
                channel->send(reinterpret_cast<const rtc::byte*>(bytes.data()), bytes.size());
            });
        const auto& status = session.controlSession->status();
        if (status.revision != session.controlReportRevision && session.channel &&
            session.channel->isOpen() && session.channel->bufferedAmount() == 0)
        {
            if (!session.live) session.channel->send(session.controlSession->statusJson());
            printf("Session control: startup %u/3; display reply %s; pings %u/%u; clock %s; "
                "offset %.1f ms; delay %lld ms%s\n", status.startupMessages,
                status.displayReply ? "yes" : "no", status.pingsAccepted, status.pingsSent,
                status.clock ? "synchronized" : "waiting", status.clock ? status.clock->offsetMs() : 0,
                static_cast<long long>(status.clock ? status.clock->delayMs : 0),
                status.closeAckSent ? "; close ACK sent" : "");
            session.controlReportRevision = status.revision;
        }
        return status.shouldClosePeer;
    }
    catch (const std::exception& error)
    {
        printf("Session control failed: %s\n", error.what());
        session.controlSession->stop();
        return true;
    }
}

void applySdpAnswer(WebRtcSession& session, const std::string& sdp)
{
    if (!session.peer || session.peer->remoteDescription())
        throw std::runtime_error("A fresh peer is required for this answer.");
    rtc::Description answer(sdp, rtc::Description::Type::Answer);
    const bool gatheringEnded = answer.ended();
    auto candidates = answer.extractCandidates();
    for (auto& candidate : candidates)
    {
        if (!candidate.resolve(rtc::Candidate::ResolveMode::Simple))
            throw std::runtime_error("Answer candidates must use numeric IP addresses.");
        const auto address = candidate.address();
        in_addr ipv4{};
        in6_addr ipv6{};
        if (!address || (inet_pton(AF_INET, address->c_str(), &ipv4) != 1 &&
                         inet_pton(AF_INET6, address->c_str(), &ipv6) != 1))
            throw std::runtime_error("ICE address formatting returned a hostname instead of an IP.");
        printf("Validated remote ICE address: %s, port %u.\n", address->c_str(),
            static_cast<unsigned>(*candidate.port()));
    }
    answer.addCandidates(std::move(candidates));
    if (gatheringEnded) answer.endCandidates();
    session.peer->setRemoteDescription(answer);
    printf("WebRTC answer applied.\n");
}

enum H264NalFlags : unsigned int
{
    H264Sps = 1u << 0,
    H264Pps = 1u << 1,
    H264Idr = 1u << 2
};

unsigned int inspectH264NalTypes(const rtc::binary& frame)
{
    unsigned int found = 0;
    size_t zeros = 0;

    for (size_t i = 0; i < frame.size(); ++i)
    {
        const unsigned int value = std::to_integer<unsigned int>(frame[i]);
        if (value == 0)
        {
            ++zeros;
            continue;
        }

        if (value == 1 && zeros >= 2 && i + 1 < frame.size())
        {
            const unsigned int header =
                std::to_integer<unsigned int>(frame[i + 1]);
            if ((header & 0x80) == 0)
            {
                switch (header & 0x1F)
                {
                    case 7: found |= H264Sps; break;
                    case 8: found |= H264Pps; break;
                    case 5: found |= H264Idr; break;
                }
            }
        }

        zeros = 0;
    }

    return found;
}

void prepareWebRtc(WebRtcSession& session, bool live = false, luna::video::Mode mode = luna::video::Mode::P720_30,
    luna::video::Decoder decoderPreference = luna::video::Decoder::Software, unsigned bitrateMbps = 0)
{
    if (session.peer)
    {
        printf("WebRTC session already running.\n");
        return;
    }

    try
    {
        const auto videoSettings = luna::video::configuration(mode);
        session.live = live;
        // Library diagnostics can contain remote SDP/ICE credentials. Native
        // auth builds report fixed application stages instead of raw messages.
        rtc::InitLogger(rtc::LogLevel::None, [](rtc::LogLevel, std::string) {});
        rtc::SetThreadPoolSize(2);
        printf("Initializing WebRTC...\n");
        rtc::Preload();
        printf("WebRTC initialized.\n");
        {
            rtc::Configuration config;
            config.disableAutoNegotiation = true;
            config.disableAutoGathering = true;

            config.bindAddress = getSwitchIpv4();
            printf("WebRTC binding to %s\n", config.bindAddress->c_str());
            session.peer = std::make_shared<rtc::PeerConnection>(config);
            auto& peer = *session.peer;
            auto gatheringDone = std::make_shared<std::promise<void>>();
            auto gatheringFuture = gatheringDone->get_future();

            peer.onGatheringStateChange(
                [gatheringDone](rtc::PeerConnection::GatheringState state)
                {
                    if (state == rtc::PeerConnection::GatheringState::Complete)
                        gatheringDone->set_value();
                });
            rtc::Description::Video video(
                "video", rtc::Description::Direction::RecvOnly);
            video.addH264Codec(108, videoSettings.fmtp);
            
            session.video = peer.addTrack(video);

            session.video->setMediaHandler(
                std::make_shared<rtc::H264RtpDepacketizer>(
                    rtc::NalUnit::Separator::LongStartSequence));

            session.video->chainMediaHandler(
                std::make_shared<rtc::RtcpReceivingSession>());

            auto videoFrames = std::make_shared<std::atomic<size_t>>(0);
            auto encodedVideoBytes = std::make_shared<std::atomic<std::uint64_t>>(0);
            session.encodedVideoBytes = encodedVideoBytes;
            auto nalTypesSeen =
                std::make_shared<std::atomic<unsigned int>>(0);
            if (!session.decoder)
                session.decoder = std::make_shared<H264Decoder>(videoSettings.width, videoSettings.height, decoderPreference, videoSettings.fps);
            auto decoder = session.decoder;

            session.video->onFrame(
                [videoFrames, nalTypesSeen, decoder, encodedVideoBytes](rtc::binary frame, rtc::FrameInfo info)
                {
                    if (frame.empty())
                        return;
                    encodedVideoBytes->fetch_add(frame.size(), std::memory_order_relaxed);

                    const unsigned int found = inspectH264NalTypes(frame);
                    const unsigned int previous = nalTypesSeen->fetch_or(found);
                    const unsigned int firstSeen = found & ~previous;

                    if (firstSeen & H264Sps)
                        printf("H.264 SPS detected.\n");
                    if (firstSeen & H264Pps)
                        printf("H.264 PPS detected.\n");
                    if (firstSeen & H264Idr)
                        printf("H.264 IDR slice detected.\n");

                    const unsigned int required = H264Sps | H264Pps | H264Idr;
                    if ((previous & required) != required &&
                        ((previous | found) & required) == required)
                    {
                        printf("H.264 SPS, PPS and IDR have all been observed.\n");
                    }

                    const size_t count = videoFrames->fetch_add(1) + 1;

                    if (count == 1 || count % 60 == 0)
                    {
                        printf("H.264 encoded frames: %zu (last: %zu bytes, RTP timestamp: %u)\n",
                            count, frame.size(),
                            static_cast<unsigned int>(info.timestamp));
                    }

                    try
                    {
                        decoder->decode(frame.data(), frame.size());
                    }
                    catch (const std::exception& error)
                    {
                        printf("H.264 decoder failed: %s\n", error.what());
                    }
                });

            rtc::Description::Audio audio(
                "audio", rtc::Description::Direction::RecvOnly);
            audio.addOpusCodec(111, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
            session.audio = peer.addTrack(audio);
            session.audio->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
            session.audio->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
            auto audioPlayer = std::make_shared<OpusAudioPlayer>();
            session.audioPlayer = audioPlayer;
            session.audio->onFrame(
                [audioPlayer](rtc::binary packet, rtc::FrameInfo info)
                {
                    if (info.payloadType != 111)
                        return;
                    try
                    {
                        audioPlayer->enqueue(packet.data(), packet.size(), info.timestamp);
                    }
                    catch (const std::exception& error)
                    {
                        printf("Opus packet queue failed: %s\n", error.what());
                    }
                });
            rtc::DataChannelInit inputConfig;
            if (live)
            {
                inputConfig.reliability.unordered = true;
                inputConfig.reliability.maxRetransmits = 0;
            }
            session.channel = peer.createDataChannel(live ? "inputChannel" : "lunanx-test", inputConfig);

            session.channel->onOpen([]()
            {
                printf("WebRTC data channel open.\n");
            });

            session.channel->onMessage(
                [](rtc::binary data)
                {
                    printf("WebRTC binary message: %zu bytes.\n", data.size());
                },
                [live](std::string text)
                {
                    if (!live) printf("WebRTC message: %s\n", text.c_str());
                });

            auto controlSession = std::make_shared<luna::control::LocalControlSession>([]
            {
                return luna::control::ClockReading{
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count()};
            }, live, mode, bitrateMbps);
            session.controlSession = controlSession;
            session.controlChannel = peer.createDataChannel("clientMessageChannel");
            session.controlChannel->onMessage([controlSession](rtc::binary frame)
            {
                controlSession->enqueue(reinterpret_cast<const std::uint8_t*>(frame.data()), frame.size());
            }, [](std::string) { printf("Ignoring text on local control channel.\n"); });

            printf("Creating WebRTC offer...\n");
            peer.setLocalDescription(rtc::Description::Type::Offer);
            printf("Gathering WebRTC ICE candidates...\n");
            peer.gatherLocalCandidates();

            if (gatheringFuture.wait_for(std::chrono::seconds(5)) !=
                std::future_status::ready)
            {
                throw std::runtime_error("ICE gathering timed out.");
            }

            gatheringFuture.get();

            const auto description = peer.localDescription();
            if (!description)
                throw std::runtime_error("No local SDP generated.");

            const std::string sdp = description->generateSdp();
            session.offerSdp = sdp;
            printf("Offer media sections: %d\n", description->mediaCount());
            printf("WebRTC offer created: %zu bytes.\n", sdp.size());
            printf("Offer has data channel: %s\n",
                description->hasApplication() ? "yes" : "no");
            printf("Offer has fingerprint: %s\n",
                description->fingerprint() ? "yes" : "no");

            printf("Offer ICE candidates: %zu\n", description->candidates().size());

            printf("WebRTC session ready.\n");
        }
    }
    catch (const std::exception& error)
    {
        (void)error;
        printf("WebRTC preparation failed.\n");
        stopWebRtc(session);
    }
}

struct NativeRuntime::Impl
{
    explicit Impl(PrintConsole* console) : view(console) {}
    WebRtcSession session;
    VideoView view;
    std::unique_ptr<RgbaImage> image;
    bool videoEnabled = true;
    luna::video::Mode mode = luna::video::Mode::P720_30;
    unsigned bitrateMbps = 0;
    std::uint64_t previousEncodedBytes = 0;
    std::optional<std::chrono::steady_clock::time_point> perfStarted;
    H264Decoder::Stats previousDecoder;
    VideoView::Stats previousView;
    luna::video::FramePacer pacer;
    struct PacingStats
    {
        std::size_t loops = 0, sleeps = 0, overruns = 0;
        std::chrono::nanoseconds work{}, sleep{}, wakeDelay{};
    } pacing, previousPacing;
    std::chrono::steady_clock::time_point nextKeyframeRequest{};
    std::function<void()> videoHandoff;
    bool answerReady = false, firstFramePresented = false, overlayEnabled = false, overlayDirty = false;
    std::unique_ptr<luna::video::PerformanceOverlay> overlayRenderer;
    const RgbaImage* overlayImage = nullptr;
    std::optional<std::chrono::steady_clock::time_point> overlayStarted;
    H264Decoder::Stats overlayDecoder;
    VideoView::Stats overlayView;
    PacingStats overlayPacing;
    std::uint64_t overlayBytes = 0;
    bool refreshOverlay(std::chrono::steady_clock::time_point now)
    {
        if (!overlayEnabled || !overlayRenderer || !session.decoder) return false;
        const auto decoded = session.decoder->getStats();
        const auto shown = view.getStats();
        const auto bytes = session.encodedVideoBytes ? session.encodedVideoBytes->load(std::memory_order_relaxed) : 0;
        if (overlayStarted && now - *overlayStarted < std::chrono::seconds(1)) return false;
        luna::video::PerformanceSample sample;
        sample.mode = mode; sample.bitrateMbps = bitrateMbps; sample.backend = decoded.backend;
        sample.fallbacks = decoded.fallbacks;
        if (overlayStarted)
        {
            sample.ready = true;
            const auto frames = decoded.framesDecoded - overlayDecoder.framesDecoded;
            const auto converted = decoded.conversionAttempts - overlayDecoder.conversionAttempts;
            const auto presented = shown.framesPresented - overlayView.framesPresented;
            const auto transfers = decoded.transferAttempts - overlayDecoder.transferAttempts;
            const auto loops = pacing.loops - overlayPacing.loops;
            const double seconds = std::chrono::duration<double>(now - *overlayStarted).count();
            const auto ms = [](auto time) { return std::chrono::duration<double, std::milli>(time).count(); };
            sample.decodedFps = frames / seconds; sample.presentedFps = presented / seconds;
            sample.encodedMbps = (bytes - overlayBytes) * 8.0 / seconds / 1'000'000;
            sample.replaced = decoded.pendingReplaced - overlayDecoder.pendingReplaced;
            sample.decodeMs = frames ? ms(decoded.decodeTime - overlayDecoder.decodeTime) / frames : 0;
            sample.transferMs = transfers ? ms(decoded.transferTime - overlayDecoder.transferTime) / transfers : 0;
            sample.rgbaMs = converted ? ms(decoded.conversionTime - overlayDecoder.conversionTime) / converted : 0;
            sample.presentMs = presented ? ms(shown.presentTime - overlayView.presentTime) / presented : 0;
            sample.loopMs = loops ? ms(pacing.work - overlayPacing.work) / loops : 0;
        }
        overlayImage = &overlayRenderer->draw(sample);
        overlayStarted = now; overlayDecoder = decoded; overlayView = shown; overlayPacing = pacing; overlayBytes = bytes;
        return true;
    }
};
NativeRuntime::NativeRuntime(PrintConsole* console) : impl_(std::make_unique<Impl>(console)) {}
NativeRuntime::~NativeRuntime() { stop(); }
void NativeRuntime::prepareVideo(luna::video::Mode mode, luna::video::Decoder decoder, unsigned bitrateMbps)
{
    if (impl_->session.peer || impl_->session.decoder) throw std::runtime_error("Fresh native decoder required");
    const auto settings = luna::video::configuration(mode);
    const auto selectedBitrate = luna::video::effectiveBitrate(mode, bitrateMbps);
    impl_->session.decoder = std::make_shared<H264Decoder>(settings.width, settings.height, decoder, settings.fps);
    impl_->mode = mode;
    impl_->bitrateMbps = selectedBitrate;
}
bool NativeRuntime::hardwareDecoderPrepared() const
{
    return impl_->session.decoder && impl_->session.decoder->hardwarePrepared();
}
bool NativeRuntime::setStatsOverlay(bool enabled)
{
    try
    {
        if (enabled && !impl_->overlayRenderer)
            impl_->overlayRenderer = std::make_unique<luna::video::PerformanceOverlay>("romfs:/fonts/Inter-Regular.ttf");
        impl_->overlayEnabled = enabled; impl_->overlayDirty = true;
        impl_->overlayImage = nullptr; impl_->overlayStarted.reset();
        return true;
    }
    catch (...) { impl_->overlayEnabled = false; printf("Performance overlay unavailable.\n"); return false; }
}
bool NativeRuntime::statsOverlayEnabled() const { return impl_->overlayEnabled; }
void NativeRuntime::setVideoHandoff(std::function<void()> handoff) { impl_->videoHandoff = std::move(handoff); }
bool NativeRuntime::hasPresentedFrame() const { return impl_->firstFramePresented; }
bool NativeRuntime::waitingForVideo() const
{ return impl_->answerReady && !impl_->firstFramePresented && impl_->session.peer && impl_->videoEnabled; }
std::string NativeRuntime::createOffer(luna::video::Mode mode, luna::video::Decoder decoder, unsigned bitrateMbps)
{
    if (impl_->session.peer) throw std::runtime_error("Fresh native peer required");
    if (!impl_->session.decoder) prepareVideo(mode, decoder, bitrateMbps);
    if (mode != impl_->mode || luna::video::effectiveBitrate(mode, bitrateMbps) != impl_->bitrateMbps)
        throw std::runtime_error("Prepared native video settings mismatch");
    const auto settings = luna::video::configuration(mode);
    impl_->mode = mode;
    impl_->view.setSize(settings.width, settings.height);
    printf("Requested native video: %s; decoder: %s.\n", settings.label, luna::video::decoderLabel(decoder));
    const auto bitrate = luna::video::bitrateRange(mode, impl_->bitrateMbps);
    printf("Requested native bitrate: min=%u max=%u start=%u bits/s; automatic=%d.\n",
        bitrate.minimum, bitrate.maximum, bitrate.start, impl_->bitrateMbps == 0);
    prepareWebRtc(impl_->session, true, mode, decoder, impl_->bitrateMbps);
    if (!impl_->session.peer || impl_->session.offerSdp.empty()) throw std::runtime_error("Offer preparation failed");
    return impl_->session.offerSdp;
}
void NativeRuntime::applyAnswer(const std::string& sdp)
{ applySdpAnswer(impl_->session, sdp); impl_->answerReady = true; }
bool NativeRuntime::visible() const { return impl_->view.isVisible(); }
bool NativeRuntime::active() const { return impl_->session.peer != nullptr; }
luna::video::Mode NativeRuntime::videoMode() const { return impl_->mode; }
std::int64_t NativeRuntime::frameIntervalNs() const { return 1'000'000'000 / luna::video::configuration(impl_->mode).fps; }
void NativeRuntime::paceFrame(std::chrono::steady_clock::time_point started)
{
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;
    if (!visible()) { impl_->pacer.reset(); return; }
    const auto now = Clock::now();
    const Duration period(frameIntervalNs());
    const auto work = std::chrono::duration_cast<Duration>(now - started);
    const auto delay = impl_->pacer.delay(started, now, period);
    ++impl_->pacing.loops;
    impl_->pacing.work += work;
    if (work > period) ++impl_->pacing.overruns;
    if (delay > Duration::zero())
    {
        ++impl_->pacing.sleeps;
        impl_->pacing.sleep += delay;
        svcSleepThread(delay.count());
        const auto excess = std::chrono::duration_cast<Duration>(Clock::now() - now) - delay;
        if (excess > Duration::zero()) impl_->pacing.wakeDelay += excess;
    }
}
void NativeRuntime::stop()
{
    if (impl_->session.peer) stopWebRtc(impl_->session);
    else impl_->session.decoder.reset(); // Also release a preflight decoder cancelled before ICE.
    impl_->view.hide(); impl_->image.reset();
    impl_->videoEnabled = true;
    impl_->perfStarted.reset();
    impl_->nextKeyframeRequest = {};
    impl_->pacer.reset(); impl_->pacing = {}; impl_->previousPacing = {};
    impl_->answerReady = impl_->firstFramePresented = impl_->overlayEnabled = impl_->overlayDirty = false;
    impl_->overlayImage = nullptr; impl_->overlayRenderer.reset(); impl_->overlayStarted.reset();
}
void NativeRuntime::update(PadState& pad, bool suppressInput, bool toggleVideo)
{
    auto& session = impl_->session;
    if (!session.peer) return;
    if (toggleVideo)
    {
        impl_->videoEnabled = !impl_->videoEnabled;
        if (!impl_->videoEnabled) { impl_->view.hide(); impl_->pacer.reset(); }
    }
    if (updateSessionControl(session) || session.peer->state() == rtc::PeerConnection::State::Failed ||
        session.peer->state() == rtc::PeerConnection::State::Closed)
    {
        stop(); printf("Native streaming connection stopped.\n"); return;
    }
    std::optional<std::int64_t> serverTime;
    if (session.controlSession && session.controlSession->status().clock)
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        serverTime = static_cast<std::int64_t>(now + session.controlSession->status().clock->offsetMs());
    }
    if (serverTime && (!session.controlSession || !session.controlSession->status().closing))
        session.controllerInput.update(pad, session.channel, serverTime, suppressInput);
    else session.controllerInput.stop(session.channel);
    if (session.audioPlayer)
    {
        try { session.audioPlayer->update(); }
        catch (...) { session.audioPlayer->stop(); printf("Native audio playback stopped.\n"); }
    }
    bool changed = false;
    if (session.decoder)
    {
        const auto now = std::chrono::steady_clock::now();
        if (session.video && session.decoder->getStats().waitingForKeyframe && now >= impl_->nextKeyframeRequest)
        {
            const bool sent = session.video->requestKeyframe();
            printf("H.264 recovery keyframe request: sent=%d.\n", sent);
            impl_->nextKeyframeRequest = now + std::chrono::seconds(1);
        }
        auto image = session.decoder->takeImage();
        if (image) { impl_->image = std::move(image); changed = true; }
    }
    const RgbaImage* overlay = nullptr;
    bool redrawOverlay = impl_->overlayDirty;
    if (impl_->videoEnabled) redrawOverlay |= impl_->refreshOverlay(std::chrono::steady_clock::now());
    overlay = impl_->overlayEnabled ? impl_->overlayImage : nullptr;
    if (impl_->videoEnabled && impl_->image && (changed || !impl_->view.isVisible() || redrawOverlay))
    {
        if (!impl_->view.isVisible() && impl_->videoHandoff) impl_->videoHandoff();
        if (!impl_->view.show(*impl_->image, overlay, changed || !impl_->firstFramePresented))
        { stop(); printf("Native video presentation stopped.\n"); return; }
        impl_->firstFramePresented = true;
        impl_->overlayDirty = false;
    }
    if (session.decoder)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto decoded = session.decoder->getStats();
        const auto presented = impl_->view.getStats();
        const auto encodedBytes = session.encodedVideoBytes ? session.encodedVideoBytes->load(std::memory_order_relaxed) : 0;
        if (impl_->perfStarted && now - *impl_->perfStarted >= std::chrono::seconds(3))
        {
            const auto frames = decoded.framesDecoded - impl_->previousDecoder.framesDecoded;
            const auto converted = decoded.conversionAttempts - impl_->previousDecoder.conversionAttempts;
            const auto shown = presented.framesPresented - impl_->previousView.framesPresented;
            const auto transfers = decoded.transferAttempts - impl_->previousDecoder.transferAttempts;
            const auto loops = impl_->pacing.loops - impl_->previousPacing.loops;
            const auto sleeps = impl_->pacing.sleeps - impl_->previousPacing.sleeps;
            const double seconds = std::chrono::duration<double>(now - *impl_->perfStarted).count();
            const auto milliseconds = [](auto duration) { return std::chrono::duration<double, std::milli>(duration).count(); };
            printf("Native video perf: decoder=%s; requested %d fps; decoded %.1f fps; presented %.1f fps; "
                "decode %.2f ms; transfer %.2f ms; RGBA %.2f ms; present %.2f ms; replaced %zu; fallbacks %zu; "
                "loop %.2f ms; sleep %.2f ms; wake delay %.2f ms; overruns %zu; encoded video %.2f Mbps\n",
                luna::video::backendLabel(decoded.backend), luna::video::configuration(impl_->mode).fps, frames / seconds, shown / seconds,
                frames ? milliseconds(decoded.decodeTime - impl_->previousDecoder.decodeTime) / frames : 0,
                transfers ? milliseconds(decoded.transferTime - impl_->previousDecoder.transferTime) / transfers : 0,
                converted ? milliseconds(decoded.conversionTime - impl_->previousDecoder.conversionTime) / converted : 0,
                shown ? milliseconds(presented.presentTime - impl_->previousView.presentTime) / shown : 0,
                decoded.pendingReplaced - impl_->previousDecoder.pendingReplaced, decoded.fallbacks,
                loops ? milliseconds(impl_->pacing.work - impl_->previousPacing.work) / loops : 0,
                loops ? milliseconds(impl_->pacing.sleep - impl_->previousPacing.sleep) / loops : 0,
                sleeps ? milliseconds(impl_->pacing.wakeDelay - impl_->previousPacing.wakeDelay) / sleeps : 0,
                impl_->pacing.overruns - impl_->previousPacing.overruns,
                (encodedBytes - impl_->previousEncodedBytes) * 8.0 / seconds / 1'000'000);
            impl_->perfStarted.reset();
        }
        if (!impl_->perfStarted && decoded.framesDecoded)
        {
            impl_->perfStarted = now; impl_->previousDecoder = decoded; impl_->previousView = presented;
            impl_->previousPacing = impl_->pacing;
            impl_->previousEncodedBytes = encodedBytes;
        }
    }
}
