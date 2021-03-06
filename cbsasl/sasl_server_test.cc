/*
 *     Copyright 2015 Couchbase, Inc.
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
#include <cbcrypto/cbcrypto.h>
#include <cbsasl/client.h>
#include <cbsasl/mechanism.h>
#include <cbsasl/server.h>
#include <folly/portability/GTest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gsl/gsl>

char envptr[1024]{"CBSASL_PWFILE=" SOURCE_ROOT "/cbsasl/sasl_server_test.json"};

static std::string mechanisms;

class SaslServerTest : public ::testing::Test {
protected:
    static void SetUpTestCase() {
        putenv(envptr);

        using namespace cb::crypto;
        if (isSupported(Algorithm::SHA512)) {
            mechanisms.append("SCRAM-SHA512 ");
        }

        if (isSupported(Algorithm::SHA256)) {
            mechanisms.append("SCRAM-SHA256 ");
        }

        if (isSupported(Algorithm::SHA1)) {
            mechanisms.append("SCRAM-SHA1 ");
        }

        mechanisms.append("PLAIN");
        cb::sasl::server::initialize();
    }

    static void TearDownTestCase() {
        cb::sasl::server::shutdown();
    }

protected:
    cb::sasl::server::ServerContext context;
};

TEST_F(SaslServerTest, ListMechs) {
    EXPECT_EQ(mechanisms, cb::sasl::server::listmech());
}

TEST_F(SaslServerTest, BadMech) {
    EXPECT_THROW(context.start("bad_mech", "", "foobar"),
                 cb::sasl::unknown_mechanism);
}

TEST_F(SaslServerTest, PlainCorrectPassword) {
    /* Normal behavior */
    auto data = context.start("PLAIN", "", {"\0mikewied\0mikepw", 16});
    EXPECT_EQ(cb::sasl::Error::OK, data.first);
    EXPECT_TRUE(data.second.empty());
}

TEST_F(SaslServerTest, PlainWrongPassword) {
    auto data = context.start("PLAIN", "", {"\0mikewied\0badpPW", 16});
    EXPECT_EQ(cb::sasl::Error::PASSWORD_ERROR, data.first);
    EXPECT_TRUE(data.second.empty());
}

TEST_F(SaslServerTest, PlainNoPassword) {
    auto data = context.start("PLAIN", "", {"\0nopass\0", 8});
    EXPECT_EQ(cb::sasl::Error::OK, data.first);
    EXPECT_TRUE(data.second.empty());
}

TEST_F(SaslServerTest, PlainWithAuthzid) {
    auto data = context.start("PLAIN", "", {"funzid\0mikewied\0mikepw", 22});
    EXPECT_EQ(cb::sasl::Error::OK, data.first);
    EXPECT_TRUE(data.second.empty());
}

TEST_F(SaslServerTest, PlainWithNoPwOrUsernameEndingNull) {
    auto data = context.start("PLAIN", "", {"funzid\0mikewied", 15});
    EXPECT_EQ(cb::sasl::Error::BAD_PARAM, data.first);
    EXPECT_TRUE(data.second.empty());
}

TEST_F(SaslServerTest, PlainNoNullAtAll) {
    auto data = context.start("PLAIN", "", {"funzidmikewied", 14});
    EXPECT_EQ(cb::sasl::Error::BAD_PARAM, data.first);
    EXPECT_TRUE(data.second.empty());
}

TEST_F(SaslServerTest, CantPickUnsupportedMechanism) {
    EXPECT_THROW(
            context.start("PLAIN", "SCRAM-SHA512", {"\0mikewied\0mikepw", 16}),
            cb::sasl::unknown_mechanism);
}
