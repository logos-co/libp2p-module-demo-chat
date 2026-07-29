#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "plugin.h"

namespace {

constexpr const char* kAppTitle = "Demo Chat";
constexpr const char* kDefaultTopic = "logos.demo-chat.v1";
constexpr const char* kDefaultListen = "/ip4/0.0.0.0/tcp/0";
constexpr const char* kStateRoot = ".demo-chat/state";

std::atomic<bool> gStop{false};

struct BootstrapPeer {
    std::string peerId;
    std::vector<std::string> addrs;
};

struct Options {
    std::string id;
    bool bootstrap = false;
    std::vector<BootstrapPeer> bootstrapPeers;
    std::string listen = kDefaultListen;
    std::string nick;
    std::string topic = kDefaultTopic;
};

void onSignal(int) {
    gStop.store(true, std::memory_order_release);
}

void printUsage(std::ostream& out) {
    out << kAppTitle << "\n"
        << "Usage:\n"
        << "  demo-chat --id <id> [--bootstrap]\n"
        << "            [--bootstrap-peer <peerId>@<multiaddr>[,<multiaddr>]]\n"
        << "            [--listen <multiaddr>] [--nick <name>] [--topic <topic>]\n";
}

std::vector<std::string> split(const std::string& value, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, delim)) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }
    return parts;
}

BootstrapPeer parseBootstrapPeer(const std::string& raw) {
    auto at = raw.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= raw.size()) {
        throw std::runtime_error("--bootstrap-peer must be <peerId>@<multiaddr>[,<multiaddr>]");
    }

    BootstrapPeer peer;
    peer.peerId = raw.substr(0, at);
    peer.addrs = split(raw.substr(at + 1), ',');
    if (peer.addrs.empty()) {
        throw std::runtime_error("--bootstrap-peer must include at least one multiaddr");
    }
    return peer;
}

std::string sanitizeId(const std::string& id) {
    if (id.empty()) {
        throw std::runtime_error("--id is required");
    }

    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_' || c == '-';
        if (!ok) {
            throw std::runtime_error("--id may contain only letters, digits, '_' and '-'");
        }
        out.push_back(c);
    }
    return out;
}

Options parseArgs(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto needValue = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(name) + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            std::exit(0);
        } else if (arg == "--id") {
            opts.id = sanitizeId(needValue("--id"));
        } else if (arg == "--bootstrap") {
            opts.bootstrap = true;
        } else if (arg == "--bootstrap-peer") {
            opts.bootstrapPeers.push_back(parseBootstrapPeer(needValue("--bootstrap-peer")));
        } else if (arg == "--listen") {
            opts.listen = needValue("--listen");
        } else if (arg == "--nick") {
            opts.nick = needValue("--nick");
        } else if (arg == "--topic") {
            opts.topic = needValue("--topic");
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    opts.id = sanitizeId(opts.id);
    if (opts.nick.empty()) {
        opts.nick = "demo-" + opts.id;
    }
    if (!opts.bootstrap && opts.bootstrapPeers.empty()) {
        throw std::runtime_error("normal chat nodes require --bootstrap-peer");
    }
    if (opts.topic.empty()) {
        throw std::runtime_error("--topic must not be empty");
    }
    return opts;
}

std::filesystem::path stateDirFor(const std::string& id) {
    return std::filesystem::path(kStateRoot) / id;
}

std::string loadOrCreatePrivateKey(const std::filesystem::path& stateDir) {
    std::filesystem::create_directories(stateDir);

    const auto identityPath = stateDir / "identity.json";
    if (std::filesystem::exists(identityPath)) {
        std::ifstream in(identityPath);
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object() || !j.contains("privKey") || !j["privKey"].is_string()) {
            throw std::runtime_error("invalid identity file: " + identityPath.string());
        }
        return j["privKey"].get<std::string>();
    }

    Libp2pModuleImpl keygen;
    auto res = keygen.newPrivateKey();
    if (!res.success || !res.value.is_string()) {
        throw std::runtime_error("failed to generate private key: " + res.error);
    }

    const std::string privKey = res.value.get<std::string>();
    nlohmann::json j;
    j["app"] = kAppTitle;
    j["privKey"] = privKey;

    const auto tmpPath = stateDir / "identity.json.tmp";
    {
        std::ofstream out(tmpPath, std::ios::trunc);
        out << j.dump(2) << "\n";
    }
    std::filesystem::permissions(
        tmpPath,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    std::filesystem::rename(tmpPath, identityPath);

    return privKey;
}

std::vector<std::string> jsonStringArray(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) {
        return out;
    }
    for (const auto& item : value) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

std::string nowUtcIso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::string displayTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

std::string peerConnectionString(const std::string& peerId, const std::vector<std::string>& addrs) {
    std::ostringstream ss;
    ss << peerId << "@";
    for (size_t i = 0; i < addrs.size(); ++i) {
        if (i > 0) {
            ss << ",";
        }
        ss << addrs[i];
    }
    return ss.str();
}

void writePeerConnectionFile(
    const std::filesystem::path& stateDir,
    const std::string& peerId,
    const std::vector<std::string>& addrs)
{
    std::ofstream out(stateDir / "bootstrap-peer.txt", std::ios::trunc);
    out << peerConnectionString(peerId, addrs) << "\n";
}

bool resultOk(const StdLogosResult& res, const std::string& action) {
    if (res.success) {
        return true;
    }
    std::cerr << kAppTitle << ": " << action << " failed: " << res.error << "\n";
    return false;
}

bool connectPeerOnce(
    Libp2pModuleImpl& node,
    const std::string& peerId,
    const std::vector<std::string>& addrs,
    std::set<std::string>& connected,
    std::mutex& outputMutex)
{
    if (peerId.empty() || addrs.empty() || connected.count(peerId) > 0) {
        return false;
    }

    auto res = node.connectPeer(peerId, addrs, 5000);
    if (!res.success) {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cerr << kAppTitle << ": connect to " << peerId << " failed: " << res.error << "\n";
        return false;
    }

    connected.insert(peerId);
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << kAppTitle << ": connected to " << peerId << "\n";
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Options opts;
    try {
        opts = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << kAppTitle << ": " << e.what() << "\n\n";
        printUsage(std::cerr);
        return 2;
    }

    const auto stateDir = stateDirFor(opts.id);
    std::string privKey;
    try {
        privKey = loadOrCreatePrivateKey(stateDir);
    } catch (const std::exception& e) {
        std::cerr << kAppTitle << ": " << e.what() << "\n";
        return 1;
    }

    Libp2pModuleOptions moduleOpts;
    moduleOpts.addrs = {opts.listen};
    try {
        moduleOpts.privKey = decodeHex(privKey);
    } catch (const std::exception& e) {
        std::cerr << kAppTitle << ": invalid saved private key: " << e.what() << "\n";
        return 1;
    }
    moduleOpts.mountGossipsub = true;
    moduleOpts.mountKad = true;
    moduleOpts.mountServiceDiscovery = true;
    moduleOpts.gossipsubTriggerSelf = true;
    for (const auto& peer : opts.bootstrapPeers) {
        moduleOpts.bootstrapNodes.push_back({peer.peerId, peer.addrs});
    }

    Libp2pModuleImpl node(moduleOpts);
    std::mutex outputMutex;
    std::set<std::string> connectedPeers;
    std::string localPeerId;

    node.emitEvent = [&](const std::string& eventName, const std::string& data) {
        if (eventName != "gossipsubMessage") {
            return;
        }

        auto event = nlohmann::json::parse(data, nullptr, false);
        if (!event.is_object() || event.value("topic", "") != opts.topic) {
            return;
        }

        auto payload = nlohmann::json::parse(event.value("data", ""), nullptr, false);
        if (!payload.is_object() || payload.value("app", "") != kAppTitle) {
            return;
        }

        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << "[" << displayTime() << "] "
                  << payload.value("nick", payload.value("id", "unknown"))
                  << ": " << payload.value("body", "") << "\n";
    };

    if (!resultOk(node.start(), "start")) {
        return 1;
    }

    auto infoRes = node.peerInfo();
    if (!resultOk(infoRes, "peerInfo") || !infoRes.value.is_object()) {
        node.stop();
        return 1;
    }

    localPeerId = infoRes.value.value("peerId", "");
    auto localAddrs = jsonStringArray(infoRes.value["addrs"]);
    connectedPeers.insert(localPeerId);
    writePeerConnectionFile(stateDir, localPeerId, localAddrs);

    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << "=== " << kAppTitle << " ===\n"
                  << kAppTitle << ": id=" << opts.id << " nick=" << opts.nick << "\n"
                  << kAppTitle << ": peerId=" << localPeerId << "\n"
                  << kAppTitle << ": topic=" << opts.topic << "\n"
                  << kAppTitle << ": connection=" << peerConnectionString(localPeerId, localAddrs) << "\n"
                  << kAppTitle << ": wrote " << (stateDir / "bootstrap-peer.txt") << "\n";
    }

    const std::string serviceId = opts.topic;
    nlohmann::json serviceDataJson;
    serviceDataJson["app"] = kAppTitle;
    serviceDataJson["version"] = 1;
    const std::string serviceData = serviceDataJson.dump();

    if (!resultOk(node.discoStartAdvertising(serviceId, serviceData), "discoStartAdvertising")) {
        node.discoStop();
        node.stop();
        return 1;
    }
    if (!resultOk(node.discoRegisterInterest(serviceId), "discoRegisterInterest")) {
        node.discoStopAdvertising(serviceId);
        node.discoStop();
        node.stop();
        return 1;
    }

    for (const auto& peer : opts.bootstrapPeers) {
        connectPeerOnce(node, peer.peerId, peer.addrs, connectedPeers, outputMutex);
    }

    if (!resultOk(node.gossipsubSubscribe(opts.topic), "gossipsubSubscribe")) {
        node.discoUnregisterInterest(serviceId);
        node.discoStopAdvertising(serviceId);
        node.discoStop();
        node.stop();
        return 1;
    }

    std::thread discoveryThread([&] {
        while (!gStop.load(std::memory_order_acquire)) {
            auto lookup = node.discoLookup(serviceId, serviceData);
            if (lookup.success && lookup.value.is_array()) {
                for (const auto& rec : lookup.value) {
                    std::string peerId = rec.value("peerId", "");
                    auto addrs = jsonStringArray(rec["addrs"]);
                    connectPeerOnce(node, peerId, addrs, connectedPeers, outputMutex);
                }
            }

            for (int i = 0; i < 30 && !gStop.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });

    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << kAppTitle << ": ready. Type a message and press Enter. Ctrl-D exits.\n";
    }

    std::string line;
    while (!gStop.load(std::memory_order_acquire) && std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }

        nlohmann::json payload;
        payload["version"] = 1;
        payload["app"] = kAppTitle;
        payload["peerId"] = localPeerId;
        payload["id"] = opts.id;
        payload["nick"] = opts.nick;
        payload["sentAt"] = nowUtcIso8601();
        payload["body"] = line;

        auto publish = node.gossipsubPublish(opts.topic, payload.dump());
        if (!publish.success) {
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cerr << kAppTitle << ": publish failed: " << publish.error << "\n";
        }
    }

    gStop.store(true, std::memory_order_release);
    if (discoveryThread.joinable()) {
        discoveryThread.join();
    }

    node.gossipsubUnsubscribe(opts.topic);
    node.discoUnregisterInterest(serviceId);
    node.discoStopAdvertising(serviceId);
    node.discoStop();
    node.stop();

    std::lock_guard<std::mutex> lock(outputMutex);
    std::cout << kAppTitle << ": stopped\n";
    return 0;
}
