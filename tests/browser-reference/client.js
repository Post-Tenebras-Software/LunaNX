"use strict";

const testFrameRate = 30;
const byId = id => document.getElementById(id);
const log = text => { byId("log").textContent += `${text}\n`; };
let peer;
let channel;
let patternStream;
let patternTimer;
let videoStatsTimer;
let videoSender;
let audioSender;
let toneContext;
let toneOscillator;
let toneStream;
let audioStatsTimer;
let controllerTimer;
let controllerReceivedAt;
let controllerPackets = 0;
let controllerConnected;
let controllerSequence;
let controllerErrors = 0;
let controllerConnects = 0;
let controllerDisconnects = 0;
let controlPeer;
let latestControlReport;

function renderControl(state) {
    const native = state.native;
    byId("control-startup").textContent = `Startup messages: ${state.stages.join(" → ") || "none"}; ` +
        `display echoes: ${state.displayEchoes}; ping replies: ${state.pings}.`;
    byId("control-close").disabled = !state.open || state.stopped || state.stages.length !== 3 || state.closeRequested;
    if (state.closeAck) byId("control-status").textContent = "Close acknowledged; local test stopped.";
    else if (state.stopped) byId("control-status").textContent = "Control channel stopped.";
    else if (state.closeRequested) byId("control-status").textContent = "Close requested; waiting for native ACK.";
    else byId("control-status").textContent = state.stages.length === 3 ?
        "Startup received; checking framed replies and native clock." : "Control channel open; receiving startup.";
    if (native) {
        const anchor = native.offsetMs + native.delayMs / 2;
        byId("control-clock").textContent = native.synchronized ?
            `Native offset: ${native.offsetMs.toFixed(1)} ms | delay: ${native.delayMs} ms | ` +
            `offset + delay/2: ${anchor.toFixed(1)} ms (expected 1000.0).` : "Native clock is waiting for a sample.";
        byId("control-errors").textContent = `Invalid frames: ${native.invalidFrames} | ` +
            `dropped frames: ${native.droppedFrames} | send errors: ${native.sendErrors}.`;
        if (!state.stopped && !state.closeRequested && native.startupMessages === 3 && native.displayReply &&
            native.synchronized && native.pingsAccepted > 0 && native.invalidFrames === 0 &&
            native.droppedFrames === 0 && native.sendErrors === 0 &&
            Math.abs(anchor - 1000) <= 0.01 && state.stages.length === 3)
            byId("control-status").textContent = "Control test passed: startup, framed display reply, and clock offset verified.";
        else if (!state.stopped && !state.closeRequested &&
            (native.invalidFrames || native.droppedFrames || native.sendErrors))
            byId("control-status").textContent = "Control test reported errors; inspect the counters and Switch log.";
    }
}

function receiveControlReport(data) {
    if (typeof data !== "string") return false;
    let report;
    try { report = JSON.parse(data); } catch { return false; }
    if (report?.type !== "lunanx-control-status") return false;
    latestControlReport = report;
    try { controlPeer?.noteNativeReport(report); }
    catch (error) { log(`Control report error: ${error.message}`); }
    return true;
}

// Show physical Switch labels for the sender's Xbox-position mapping.
const controllerButtons = [
    [0x1000, "B"], [0x2000, "A"], [0x4000, "Y"], [0x8000, "X"],
    [0x0040, "Left stick"], [0x0080, "Right stick"], [0x0100, "L"], [0x0200, "R"],
    [0x0010, "Plus"], [0x0020, "Minus"], [0x0004, "Left"], [0x0001, "Up"],
    [0x0008, "Right"], [0x0002, "Down"]
].map(([mask, label]) => {
    const element = document.createElement("span");
    element.className = "controller-button";
    element.textContent = label;
    byId("controller-buttons").appendChild(element);
    return { mask, label, element };
});

function showControllerState(buttons, leftStick, rightStick, leftTrigger = 0, rightTrigger = 0) {
    const held = [];
    for (const button of controllerButtons) {
        const pressed = (buttons & button.mask) !== 0;
        button.element.classList.toggle("pressed", pressed);
        if (pressed) held.push(button.label);
    }
    if (leftTrigger) held.push("ZL");
    if (rightTrigger) held.push("ZR");
    byId("controller-held").textContent = `Held: ${held.length ? held.join(", ") : "none"}.`;
    byId("controller-triggers").textContent = `ZL: ${leftTrigger}/255 | ZR: ${rightTrigger}/255`;
    for (const [name, [x, y]] of [["left", leftStick], ["right", rightStick]]) {
        const dot = byId(`${name}-stick-dot`);
        dot.setAttribute("cx", x.toFixed(3));
        dot.setAttribute("cy", (-y).toFixed(3));
        byId(`${name}-stick-values`).textContent = `X: ${x.toFixed(3)} | Y: ${y.toFixed(3)}`;
    }
}

function stopControllerTest(message = "Controller test stopped; controls released.") {
    clearInterval(controllerTimer);
    controllerTimer = undefined;
    controllerReceivedAt = undefined;
    controllerConnected = undefined;
    controllerSequence = undefined;
    showControllerState(0, [0, 0], [0, 0]);
    byId("controller-status").textContent = message;
    byId("controller-connection").textContent = "No active controller channel.";
    byId("controller-wire").textContent = "No current binary input.";
}

function startControllerTest() {
    stopControllerTest("Data channel open; waiting for controller updates.");
    controllerPackets = 0;
    controllerErrors = 0;
    controllerConnects = 0;
    controllerDisconnects = 0;
    byId("controller-connection").textContent = "Waiting for controller connection event.";
    controllerTimer = setInterval(() => {
        if (controllerReceivedAt !== undefined && performance.now() - controllerReceivedAt > 750) {
            controllerReceivedAt = undefined;
            showControllerState(0, [0, 0], [0, 0]);
            byId("controller-status").textContent = "Controller updates stopped; controls released.";
            byId("controller-wire").textContent = "No current binary input.";
        }
    }, 100);
}

function receiveControllerState(data) {
    if (typeof data === "string") return false; // Hello messages keep using text.
    try {
        const message = LunaInput.decode(data);
        if (message.kind === "ack") return true;
        if (message.deviceId !== 0) throw new Error("This test expects controller 0");
        if (message.kind === "connection") {
            controllerConnected = message.connected;
            controllerSequence = undefined;
            controllerReceivedAt = undefined;
            if (message.connected) controllerConnects++;
            else controllerDisconnects++;
            showControllerState(0, [0, 0], [0, 0]);
            byId("controller-connection").textContent =
                `Controller 0 ${message.connected ? "connected" : "disconnected"}; ` +
                `connect events: ${controllerConnects}; disconnect events: ${controllerDisconnects}.`;
            byId("controller-status").textContent = message.connected ?
                "Controller connected; waiting for binary states." : "Controller disconnected; controls released.";
            return true;
        }
        if (controllerConnected === false) return true;
        if (controllerSequence !== undefined) {
            const advance = (message.packetNumber - controllerSequence) >>> 0;
            if (advance === 0 || advance >= 0x80000000) return true;
        }
        controllerSequence = message.packetNumber;
        controllerReceivedAt = performance.now();
        controllerPackets++;
        const axis = value => (value - 32767) / 32767;
        showControllerState(message.buttons, [axis(message.leftX), axis(message.leftY)],
            [axis(message.rightX), axis(message.rightY)], message.leftTrigger, message.rightTrigger);
        byId("controller-status").textContent =
            `Decoded ${controllerPackets} binary states; packet ${message.packetNumber}; controller connected.`;
        byId("controller-wire").textContent =
            `Last state: ${message.bytes} bytes | buttons 0x${message.buttons.toString(16)} | ` +
            `left [${message.leftX}, ${message.leftY}] | right [${message.rightX}, ${message.rightY}]`;
        byId("controller-timing").textContent = message.timing ?
            `Switch clock: ${message.timing.timestampMs} ms since epoch; ` +
            `hardware class ${message.timing.hardwareClass}. Local test clock; no Luna time synchronization.` :
            "State has no latency metadata.";
    } catch (error) {
        showControllerState(0, [0, 0], [0, 0]);
        byId("controller-status").textContent = `Invalid binary input; controls released: ${error.message}`;
        if (++controllerErrors === 1 || controllerErrors % 120 === 0) log(`Input decode error: ${error.message}`);
    }
    return true;
}

function stopMediaTest() {
    stopControllerTest();
    controlPeer?.stop();
    clearInterval(patternTimer);
    clearInterval(videoStatsTimer);
    patternTimer = undefined;
    videoStatsTimer = undefined;
    patternStream?.getTracks().forEach(track => track.stop());
    patternStream = undefined;
    videoSender = undefined;
    clearInterval(audioStatsTimer);
    audioStatsTimer = undefined;
    try { toneOscillator?.stop(); } catch {}
    toneOscillator?.disconnect();
    toneStream?.getTracks().forEach(track => track.stop());
    toneContext?.close().catch(() => {});
    toneOscillator = undefined;
    toneStream = undefined;
    toneContext = undefined;
    audioSender = undefined;
    byId("resume-tone").hidden = true;
}

function startTestPattern() {
    const canvas = byId("pattern");
    const context = canvas.getContext("2d");
    if (!context || !canvas.captureStream)
        throw new Error("Canvas video capture is unavailable in this browser.");
    canvas.hidden = false;
    // Keep the pattern's proportions and motion as the canvas resolution changes.
    const designWidth = 640;
    const designHeight = 360;
    context.setTransform(canvas.width / designWidth, 0, 0, canvas.height / designHeight, 0, 0);
    const colors = ["#eee", "#e3d535", "#36cddd", "#36c559", "#d63ebf", "#d53c3c", "#354ac5"];
    let frame = 0;
    const draw = () => {
        colors.forEach((color, index) => {
            context.fillStyle = color;
            const left = Math.floor(index * designWidth / colors.length);
            const right = Math.floor((index + 1) * designWidth / colors.length);
            context.fillRect(left, 0, right - left, designHeight);
        });
        context.fillStyle = "#111";
        context.fillRect(0, 270, designWidth, 90);
        context.fillStyle = "white";
        context.font = "24px monospace";
        context.fillText("LunaNX H.264 test", 20, 305);
        context.fillText(`Frame ${frame}`, 20, 340);
        context.beginPath();
        context.arc(35 + (frame * 6) % (designWidth - 70), 135, 30, 0, Math.PI * 2);
        context.fill();
        frame++;
    };
    draw();
    patternStream = canvas.captureStream(testFrameRate);
    patternTimer = setInterval(draw, 1000 / testFrameRate);
    return patternStream;
}

async function updateVideoStats(sender) {
    try {
        const stats = await sender.getStats();
        if (videoSender !== sender) return;
        for (const report of stats.values()) {
            if (report.type !== "outbound-rtp" ||
                (report.kind !== "video" && report.mediaType !== "video")) continue;
            const codec = stats.get(report.codecId);
            byId("video-stats").textContent =
                `Browser sent ${report.packetsSent || 0} RTP packets; ` +
                `encoded ${report.framesEncoded || 0} frames; codec: ${codec?.mimeType || "pending"}.`;
        }
    } catch (error) {
        if (videoSender === sender) log(`Video stats: ${error.message}`);
    }
}

async function attachTestVideo(connection) {
    if (!byId("send-video").checked) {
        byId("video-stats").textContent = "Video disabled.";
        return;
    }

    const video = connection.getTransceivers().find(item => item.receiver.track.kind === "video");
    if (!video) throw new Error("The Switch offer has no video track.");
    const h264 = (RTCRtpSender.getCapabilities("video")?.codecs || [])
        .filter(codec => codec.mimeType.toLowerCase() === "video/h264");
    if (h264.length === 0)
        throw new Error("This browser does not advertise an H.264 video encoder.");

    video.setCodecPreferences(h264);
    const stream = startTestPattern();
    await video.sender.replaceTrack(stream.getVideoTracks()[0]);
    video.sender.setStreams(stream);
    video.direction = "sendonly";
    videoSender = video.sender;
    byId("video-stats").textContent = "H.264 pattern attached; waiting for the Switch to connect.";
    const { width, height } = byId("pattern");
    log(`Video: attached H.264 canvas pattern, ${width}x${height} at ${testFrameRate} fps.`);
}

async function startTestTone() {
    const AudioContext = window.AudioContext || window.webkitAudioContext;
    if (!AudioContext) throw new Error("Web Audio is unavailable in this browser.");
    const options = { sampleRate: 48000 };
    // A network-only tone needs a running clock, without a desktop output device.
    if ("sinkId" in AudioContext.prototype) options.sinkId = { type: "none" };
    toneContext = new AudioContext(options);
    const context = toneContext;
    context.addEventListener("statechange", () => {
        if (toneContext !== context) return;
        byId("resume-tone").hidden = context.state === "running" || context.state === "closed";
        log(`Tone context: ${context.state}`);
    });
    log(`Tone output: ${context.sinkId?.type === "none" ? "silent sink" : "default audio device"}.`);
    const destination = toneContext.createMediaStreamDestination();
    destination.channelCount = 2;
    const gain = toneContext.createGain();
    gain.gain.value = 0.05;
    toneOscillator = toneContext.createOscillator();
    toneOscillator.type = "sine";
    toneOscillator.frequency.value = 440;
    toneOscillator.connect(gain);
    gain.connect(destination);
    toneStream = destination.stream;
    toneOscillator.start();
    await toneContext.resume();
    if (toneContext.state !== "running")
        throw new Error("The browser suspended the test tone. Allow audio for this page and retry.");
}

async function attachTestAudio(connection) {
    if (!byId("send-audio").checked) {
        byId("audio-stats").textContent = "Audio disabled.";
        return;
    }
    const audio = connection.getTransceivers().find(item => item.receiver.track.kind === "audio");
    if (!audio) throw new Error("The Switch offer has no audio track.");
    const opus = (RTCRtpSender.getCapabilities("audio")?.codecs || [])
        .filter(codec => codec.mimeType.toLowerCase() === "audio/opus");
    if (opus.length === 0) throw new Error("This browser does not advertise an Opus encoder.");
    audio.setCodecPreferences(opus);
    await audio.sender.replaceTrack(toneStream.getAudioTracks()[0]);
    audio.sender.setStreams(toneStream);
    audio.direction = "sendonly";
    audioSender = audio.sender;
    byId("audio-stats").textContent = "440 Hz tone attached; waiting for the Switch to connect.";
    log("Audio: attached quiet 440 Hz Opus tone, 48000 Hz stereo source.");
}

async function updateAudioStats(sender) {
    try {
        const stats = await sender.getStats();
        if (audioSender !== sender) return;
        const track = sender.track;
        const transceiver = peer?.getTransceivers().find(item => item.sender === sender);
        const encodings = sender.getParameters().encodings || [];
        const active = encodings.length ? encodings.some(item => item.active !== false) : "unknown";
        const source = [...stats.values()].find(report =>
            report.type === "media-source" && report.kind === "audio");
        const energy = source?.totalAudioEnergy;
        byId("audio-details").textContent =
            `Tone: ${toneContext?.state || "missing"}; clock: ${toneContext?.currentTime.toFixed(2) ?? "n/a"} s; ` +
            `track: ${track?.readyState || "missing"}, enabled: ${track?.enabled ?? "n/a"}, muted: ${track?.muted ?? "n/a"}; ` +
            `direction: ${transceiver?.currentDirection || "pending"}; encoding active: ${active}; ` +
            `source energy: ${energy === undefined ? "n/a" : energy.toFixed(6)}.`;
        for (const report of stats.values()) {
            if (report.type !== "outbound-rtp" ||
                (report.kind !== "audio" && report.mediaType !== "audio")) continue;
            const codec = stats.get(report.codecId);
            byId("audio-stats").textContent =
                `Browser sent ${report.packetsSent || 0} audio RTP packets; ` +
                `codec: ${codec?.mimeType || "pending"}.`;
        }
    } catch (error) {
        if (audioSender === sender) log(`Audio stats: ${error.message}`);
    }
}

function isIpv4(address) {
    const parts = address.split(".");
    return parts.length === 4 && parts.every(part =>
        /^(0|[1-9]\d{0,2})$/.test(part) && Number(part) <= 255);
}

function answerForSwitch(sdp, computerIp) {
    if (!isIpv4(computerIp))
        throw new Error("Enter your Fedora Wi-Fi IPv4 address.");

    let candidates = 0;
    let reportedCandidates = 0;
    const lines = sdp.trim().split(/\r?\n/).flatMap(line => {
        if (!line.startsWith("a=candidate:")) return [line];
        reportedCandidates++;
        const fields = line.trim().split(/\s+/);
        if (fields[2]?.toLowerCase() !== "udp") return [];
        if (/\.local\.?$/i.test(fields[4] || ""))
            fields[4] = computerIp;
        if (!fields[4] || !isIpv4(fields[4])) return [];
        candidates++;
        return [fields.join(" ")];
    });
    if (candidates === 0) {
        if (reportedCandidates === 0)
            throw new Error("Browser SDP contains no ICE candidates. Check the browser's WebRTC UDP policy.");
        throw new Error(`Browser reported ${reportedCandidates} candidates, but none are usable IPv4 UDP candidates. See the candidate log.`);
    }
    return lines.join("\r\n") + "\r\n";
}

function waitForGathering(connection) {
    if (connection.iceGatheringState === "complete") return Promise.resolve();
    return new Promise((resolve, reject) => {
        const finish = () => {
            clearTimeout(timer);
            connection.removeEventListener("icegatheringstatechange", changed);
        };
        const changed = () => {
            if (connection.iceGatheringState === "complete") {
                finish();
                resolve();
            }
        };
        const timer = setTimeout(() => {
            finish();
            reject(new Error("Browser ICE gathering timed out."));
        }, 15000);
        connection.addEventListener("icegatheringstatechange", changed);
        changed();
    });
}

function sendHello() {
    if (!channel || channel.readyState !== "open")
        throw new Error("The data channel is not open yet.");
    const message = "Hello from Chromium!";
    channel.send(message);
    log(`Sent: ${message}`);
}

byId("create").addEventListener("click", async () => {
    byId("create").disabled = true;
    byId("send").disabled = true;
    byId("send-video").disabled = true;
    byId("send-audio").disabled = true;
    try {
        const computerIp = byId("computer-ip").value.trim();
        if (!isIpv4(computerIp)) throw new Error("Enter your Fedora Wi-Fi IPv4 address.");
        const offer = byId("offer").value.trim().replace(/\r?\n/g, "\r\n") + "\r\n";
        if (!offer.startsWith("v=0\r\n"))
            throw new Error("Paste only SDP, starting with v=0, without the log markers.");
        if (!window.RTCPeerConnection) throw new Error("WebRTC is unavailable in this browser.");

        // Resume during the button gesture, before signaling awaits consume user activation.
        if (byId("send-audio").checked) await startTestTone();
        const connection = new RTCPeerConnection({ iceServers: [], bundlePolicy: "max-bundle" });
        peer = connection;
        controlPeer = undefined;
        latestControlReport = undefined;
        byId("control-status").textContent = "Waiting for clientMessageChannel.";
        byId("control-startup").textContent = "Startup messages: none.";
        byId("control-clock").textContent = "Waiting for a native clock report.";
        byId("control-errors").textContent = "Invalid frames: 0 | dropped frames: 0 | send errors: 0.";
        byId("control-close").disabled = true;
        connection.oniceconnectionstatechange = () => {
            if (peer === connection) log(`ICE: ${connection.iceConnectionState}`);
        };
        connection.onconnectionstatechange = () => {
            if (peer !== connection) return;
            log(`Connection: ${connection.connectionState}`);
            if (connection.connectionState === "closed" || connection.connectionState === "failed") {
                stopMediaTest();
                byId("video-stats").textContent = "Video test stopped.";
                byId("audio-stats").textContent = "Audio test stopped.";
                byId("create").disabled = false;
                byId("send-video").disabled = false;
                byId("send-audio").disabled = false;
            }
        };
        peer.onicegatheringstatechange = () => log(`Gathering: ${peer.iceGatheringState}`);
        peer.onicecandidateerror = event =>
            log(`Candidate error ${event.errorCode}: ${event.errorText}`);
        peer.ondatachannel = event => {
            const incoming = event.channel;
            if (peer !== connection) { incoming.close(); return; }
            incoming.binaryType = "arraybuffer";
            log(`Received data channel: ${incoming.label}`);
            if (incoming.label === "clientMessageChannel") {
                const local = new LunaControl.LocalPeer({
                    send: bytes => {
                        if (incoming.readyState !== "open") throw new Error("Control channel is not open");
                        incoming.send(bytes);
                    },
                    changed: state => { if (controlPeer === local) renderControl(state); },
                    acknowledged: () => {
                        if (peer !== connection) return;
                        connection.close();
                        stopMediaTest();
                        byId("status").textContent = "Control close acknowledged. Create a fresh offer for another test.";
                        byId("create").disabled = false;
                        byId("send-video").disabled = false;
                        byId("send-audio").disabled = false;
                    },
                });
                controlPeer = local;
                if (latestControlReport) local.noteNativeReport(latestControlReport);
                incoming.onopen = () => { if (peer === connection) local.open(); };
                incoming.onmessage = event => {
                    if (peer !== connection) return;
                    try { local.receive(event.data); }
                    catch (error) {
                        byId("control-status").textContent = `Control test error: ${error.message}`;
                        log(`Control test error: ${error.message}`);
                    }
                };
                incoming.onclose = () => local.stop();
                incoming.onerror = event => log(`Control channel error: ${event.error?.message || "unknown"}`);
                if (incoming.readyState === "open") incoming.onopen();
                return;
            }
            if (incoming.label !== "lunanx-test") {
                log(`Ignoring unsupported data channel: ${incoming.label}`);
                incoming.close();
                return;
            }
            channel = incoming;
            let announced = false;
            incoming.onopen = () => {
                if (peer !== connection) return;
                if (announced) return;
                announced = true;
                byId("send").disabled = false;
                byId("status").textContent = "Data channel open. A hello was sent to LunaNX.";
                log("Data channel open.");
                startControllerTest();
                sendHello();
            };
            incoming.onmessage = event => {
                if (peer !== connection) return;
                if (!receiveControlReport(event.data) && !receiveControllerState(event.data)) log(`Received: ${event.data}`);
            };
            incoming.onclose = () => {
                if (peer !== connection) return;
                byId("send").disabled = true;
                stopControllerTest("Data channel closed; controls released.");
                log("Data channel closed.");
            };
            incoming.onerror = event => log(`Data channel error: ${event.error?.message || "unknown"}`);
            if (incoming.readyState === "open") incoming.onopen();
        };

        byId("status").textContent = "Creating answer and gathering candidates...";
        await peer.setRemoteDescription({ type: "offer", sdp: offer });
        for (const transceiver of peer.getTransceivers()) transceiver.direction = "inactive";
        await attachTestVideo(peer);
        await attachTestAudio(peer);
        await peer.setLocalDescription(await peer.createAnswer());
        if (videoSender) {
            const parameters = videoSender.getParameters();
            if (parameters.encodings?.length) {
                parameters.encodings[0].maxBitrate = 800000;
                parameters.encodings[0].maxFramerate = testFrameRate;
                await videoSender.setParameters(parameters);
            }
            const sender = videoSender;
            videoStatsTimer = setInterval(() => updateVideoStats(sender), 2000);
        }
        if (audioSender) {
            const parameters = audioSender.getParameters();
            if (parameters.encodings?.length) {
                parameters.encodings[0].maxBitrate = 64000;
                await audioSender.setParameters(parameters);
            }
            const sender = audioSender;
            audioStatsTimer = setInterval(() => updateAudioStats(sender), 2000);
        }
        await waitForGathering(peer);
        const browserAnswer = peer.localDescription.sdp;
        if (videoSender && /(?:^|\r?\n)m=video 0(?:\s|$)/.test(browserAnswer))
            throw new Error("The browser rejected the offered H.264 video format.");
        if (audioSender && /(?:^|\r?\n)m=audio 0(?:\s|$)/.test(browserAnswer))
            throw new Error("The browser rejected the offered Opus audio format.");
        byId("answer").value = browserAnswer;
        const browserCandidates = browserAnswer.split(/\r?\n/)
            .filter(line => line.startsWith("a=candidate:"));
        log(`Browser SDP candidates: ${browserCandidates.length}`);
        for (const candidate of browserCandidates) log(candidate);
        const answer = answerForSwitch(browserAnswer, computerIp);
        const response = await fetch("/local-answer", {
            method: "POST", credentials: "omit", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ offer, answer })
        });
        if (!response.ok) throw new Error(`Saving answer failed: HTTP ${response.status}`);
        byId("answer").value = answer;
        byId("status").textContent = "Answer saved. Press ZR for the local job test, or Minus to apply directly. Keep this page open.";
        log("Answer saved. Waiting for LunaNX to apply it.");
    } catch (error) {
        peer?.close();
        stopMediaTest();
        byId("status").textContent = error.message;
        log(`Error: ${error.message}`);
        byId("create").disabled = false;
        byId("send-video").disabled = false;
        byId("send-audio").disabled = false;
    }
});

byId("send").addEventListener("click", () => {
    try { sendHello(); } catch (error) { log(`Error: ${error.message}`); }
});
byId("control-close").addEventListener("click", () => {
    try { controlPeer?.requestClose(); }
    catch (error) { log(`Control close failed: ${error.message}`); }
});
byId("resume-tone").addEventListener("click", async () => {
    const context = toneContext;
    if (!context || context.state === "closed") return;
    try {
        await context.resume();
        if (toneContext !== context) return;
        byId("resume-tone").hidden = context.state === "running";
        if (audioSender) await updateAudioStats(audioSender);
    } catch (error) {
        log(`Resuming tone failed: ${error.message}`);
    }
});
window.addEventListener("beforeunload", () => {
    peer?.close();
    stopMediaTest();
});
