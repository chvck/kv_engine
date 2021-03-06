/*
 *     Copyright 2019 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "node.h"
#include <folly/portability/Unistd.h>
#include <nlohmann/json.hpp>
#include <platform/dirutils.h>
#include <platform/strerror.h>
#include <protocol/connection/client_connection_map.h>
#ifndef WIN32
#include <signal.h>
#include <sys/wait.h>
#endif
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace cb {
namespace test {

Node::~Node() = default;
Node::Node(std::string directory) : directory(std::move(directory)) {
}

class NodeImpl : public Node {
public:
    NodeImpl(std::string directory, std::string id);
    ~NodeImpl() override;
    void startMemcachedServer();

    bool isRunning() const override;

    std::unique_ptr<MemcachedConnection> getConnection() override;

protected:
    void parsePortnumberFile();

    std::string configfile;
#ifdef WIN32
    mutable HANDLE child = INVALID_HANDLE_VALUE;
#else
    mutable pid_t child = 0;
#endif
    nlohmann::json config;
    ConnectionMap connectionMap;
    const std::string id;
};

NodeImpl::NodeImpl(std::string directory, std::string id)
    : Node(std::move(directory)), id(std::move(id)) {
    const auto errmaps =
            cb::io::sanitizePath(SOURCE_ROOT "/etc/couchbase/kv/error_maps");
    const auto rbac = cb::io::sanitizePath(SOURCE_ROOT
                                           "/tests/testapp_cluster/rbac.json");
    const auto log_filename =
            cb::io::sanitizePath(NodeImpl::directory + "/memcached_log");
    const auto portnumber_file =
            cb::io::sanitizePath(NodeImpl::directory + "/memcached.ports.json");
    const auto minidump_dir =
            cb::io::sanitizePath(NodeImpl::directory + "/crash");
    cb::io::mkdirp(minidump_dir);

    config = {{"max_connections", 1000},
              {"system_connections", 250},
              {"stdin_listener", false},
              {"datatype_json", true},
              {"datatype_snappy", true},
              {"xattr_enabled", true},
              {"dedupe_nmvb_maps", false},
              {"active_external_users_push_interval", "30 m"},
              {"error_maps_dir", errmaps},
              {"external_auth_service", true},
              {"rbac_file", rbac},
              {"ssl_cipher_list", "HIGH"},
              {"ssl_minimum_protocol", "tlsv1"},
              {"opcode_attributes_override",
               {{"version", 1}, {"EWB_CTL", {{"slow", 50}}}}},
              {"logger",
               {{"unit_test", true},
                {"console", false},
                {"filename", log_filename}}},
              {"breakpad",
               {{"enabled", true},
                {"minidump_dir", minidump_dir},
                {"content", "default"}}},
              {"portnumber_file", portnumber_file},
              {"parent_identifier", (int)getpid()}};
    config["interfaces"][0] = {{"tag", "plain"},
                               {"system", true},
                               {"port", 0},
                               {"ipv4", "required"},
                               {"host", "*"}};
    configfile = cb::io::sanitizePath(NodeImpl::directory + "/memcached.json");
    std::ofstream out(configfile);
    out << config.dump(2);
    out.close();
}

void NodeImpl::startMemcachedServer() {
#ifdef WIN32
    STARTUPINFO sinfo{};
    PROCESS_INFORMATION pinfo{};
    sinfo.cb = sizeof(sinfo);

    char commandline[1024];
    sprintf(commandline, "memcached.exe -C %s", configfile.c_str());
    if (!CreateProcess("memcached.exe", // lpApplicationName
                       commandline, // lpCommandLine
                       nullptr, // lpProcessAttributes
                       nullptr, // lpThreadAttributes
                       false, // bInheritHandles
                       0, // dwCreationFlags
                       nullptr, // lpEnvironment
                       nullptr, // lpCurrentDirectory
                       &sinfo, // lpStartupInfo
                       &pinfo)) { // lpProcessInfoqrmation
        throw std::system_error(GetLastError(),
                                std::system_category(),
                                "Failed to execute memcached");
    }

    child = pinfo.hProcess;
#else
    child = fork();
    if (child == -1) {
        throw std::system_error(
                errno, std::system_category(), "Failed to start client");
    }

    if (child == 0) {
        std::string binary(OBJECT_ROOT);
        binary.append("/memcached");

        const char* argv[20];
        int arg = 0;

        argv[arg++] = binary.c_str();
        argv[arg++] = "-C";
        argv[arg++] = configfile.c_str();

        argv[arg++] = nullptr;
        execvp(argv[0], const_cast<char**>(argv));
        throw std::system_error(
                errno, std::system_category(), "Failed to execute memcached");
    }
#endif

    // wait and read the portnumber file
    parsePortnumberFile();
}

NodeImpl::~NodeImpl() {
    if (isRunning()) {
#ifdef WIN32
        // @todo This should be made a bit more robust
        TerminateProcess(child, 0);
        WaitForSingleObject(child, 60000);
        DWORD status;
        GetExitCodeProcess(child, &status);
#else
        // Start by giving it a slow and easy start...
        const auto timeout =
                std::chrono::steady_clock::now() + std::chrono::seconds(15);
        kill(child, SIGTERM);

        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (isRunning() && std::chrono::steady_clock::now() < timeout);

        if (isRunning()) {
            // no mercy!
            kill(child, SIGKILL);

            int status;
            pid_t ret;
            while (true) {
                ret = waitpid(child, &status, 0);
                if (ret == reinterpret_cast<pid_t>(-1) && errno == EINTR) {
                    // Just loop again
                    continue;
                }
                break;
            }
        }
#endif
    }

    if (!configfile.empty()) {
        cb::io::rmrf(configfile);
    }
}

void NodeImpl::parsePortnumberFile() {
    connectionMap.initialize(nlohmann::json::parse(cb::io::loadFile(
            config["portnumber_file"], std::chrono::minutes{5})));
    cb::io::rmrf(config["portnumber_file"]);
}

#ifdef WIN32
bool NodeImpl::isRunning() const {
    if (child != INVALID_HANDLE_VALUE) {
        DWORD status;

        if (!GetExitCodeProcess(child, &status)) {
            throw std::system_error(
                    GetLastError(),
                    std::system_category(),
                    "NodeImpl::isRunning: GetExitCodeProcess failed");

            std::cerr << "GetExitCodeProcess: failed: " << cb_strerror()
                      << std::endl;
            exit(EXIT_FAILURE);
        }

        if (status == STILL_ACTIVE) {
            return true;
        }

        CloseHandle(child);
        child = INVALID_HANDLE_VALUE;
    }
    return false;
}
#else
bool NodeImpl::isRunning() const {
    if (child != 0) {
        int status;
        auto next = waitpid(child, &status, WNOHANG);
        if (next == static_cast<pid_t>(-1)) {
            throw std::system_error(errno,
                                    std::system_category(),
                                    "NodeImpl::isRunning: waitpid failed");
        }

        if (next == child) {
            child = 0;
            return false;
        }

        return true;
    }

    return false;
}
#endif

std::unique_ptr<MemcachedConnection> NodeImpl::getConnection() {
    auto ret = connectionMap.getConnection().clone();
    ret->setAutoRetryTmpfail(true);
    std::vector<cb::mcbp::Feature> features = {
            {cb::mcbp::Feature::MUTATION_SEQNO,
             cb::mcbp::Feature::XATTR,
             cb::mcbp::Feature::XERROR,
             cb::mcbp::Feature::JSON}};
    ret->setFeatures("cluster_testapp", features);
    return ret;
}

std::unique_ptr<Node> Node::create(const std::string& directory,
                                   const std::string& id) {
    auto ret = std::make_unique<NodeImpl>(directory, id);
    ret->startMemcachedServer();
    return ret;
}

} // namespace test
} // namespace cb
