/*
 *     Copyright 2020 Couchbase, Inc.
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

#include "clustertest.h"

#include "auth_provider_service.h"
#include "bucket.h"
#include "cluster.h"
#include <include/mcbp/protocol/unsigned_leb128.h>
#include <protocol/connection/client_connection.h>
#include <protocol/connection/client_mcbp_commands.h>
#include <protocol/connection/frameinfo.h>
#include <thread>

class CollectionsTests : public cb::test::ClusterTest {
protected:
    static std::unique_ptr<MemcachedConnection> getConnection() {
        auto bucket = cluster->getBucket("default");
        auto conn = bucket->getConnection(Vbid(0));
        conn->authenticate("@admin", "password", "PLAIN");
        conn->selectBucket(bucket->getName());
        conn->setFeature(cb::mcbp::Feature::Collections, true);
        return conn;
    }

    static void mutate(MemcachedConnection& conn,
                       std::string id,
                       MutationType type) {
        Document doc{};
        doc.value = "body";
        doc.info.id = std::move(id);
        doc.info.datatype = cb::mcbp::Datatype::Raw;
        const auto info = conn.mutate(doc, Vbid{0}, type);
        EXPECT_NE(0, info.cas);
    }
};

// Verify that I can store documents within the collections
TEST_F(CollectionsTests, TestBasicOperations) {
    auto conn = getConnection();
    mutate(*conn,
           createKey(Collection::Fruit, "TestBasicOperations"),
           MutationType::Add);
    mutate(*conn,
           createKey(Collection::Fruit, "TestBasicOperations"),
           MutationType::Set);
    mutate(*conn,
           createKey(Collection::Fruit, "TestBasicOperations"),
           MutationType::Replace);
}

TEST_F(CollectionsTests, TestUnknownScope) {
    auto conn = getConnection();
    try {
        mutate(*conn, createKey(1, "TestBasicOperations"), MutationType::Add);
        FAIL() << "Invalid scope not detected";
    } catch (const ConnectionError& e) {
        EXPECT_EQ(cb::mcbp::Status::UnknownCollection, e.getReason());
    }
}

TEST_F(CollectionsTests, TestBasicRbac) {
    const std::string username{"TestBasicRbac"};
    const std::string password{"TestBasicRbac"};
    cluster->getAuthProviderService().upsertUser({username, password, R"({
  "buckets": {
    "default": {
      "scopes": {
        "0": {
          "collections": {
            "0": {
              "privileges": [
                "Read"
              ]
            },
            "8": {
              "privileges": [
                "Read",
                "Insert",
                "Delete",
                "Upsert"
              ]
            }
          }
        }
      },
      "privileges": [
        "SimpleStats"
      ]
    }
  },
  "privileges": [],
  "domain": "external"
})"_json});

    auto conn = getConnection();
    mutate(*conn,
           createKey(Collection::Default, "TestBasicRbac"),
           MutationType::Add);
    mutate(*conn,
           createKey(Collection::Fruit, "TestBasicRbac"),
           MutationType::Add);
    mutate(*conn,
           createKey(Collection::Vegetable, "TestBasicRbac"),
           MutationType::Add);

    // I'm allowed to read from the default collection and read and write
    // to the fruit collection
    conn->authenticate(username, password);
    conn->selectBucket("default");

    conn->get(createKey(Collection::Default, "TestBasicRbac"), Vbid{0});
    conn->get(createKey(Collection::Fruit, "TestBasicRbac"), Vbid{0});
    try {
        conn->get(createKey(Collection::Vegetable, "TestBasicRbac"), Vbid{0});
        FAIL() << "Should not be able to fetch in vegetable collection";
    } catch (const ConnectionError& error) {
        ASSERT_TRUE(error.isAccessDenied());
    }

    // I'm only allowed to write in Fruit
    mutate(*conn,
           createKey(Collection::Fruit, "TestBasicRbac"),
           MutationType::Set);
    for (auto c : std::vector<Collection>{
                 {Collection::Default, Collection::Vegetable}}) {
        try {
            mutate(*conn, createKey(c, "TestBasicRbac"), MutationType::Set);
            FAIL() << "Should only be able to store in the fruit collection";
        } catch (const ConnectionError& error) {
            ASSERT_TRUE(error.isAccessDenied());
        }
    }

    cluster->getAuthProviderService().removeUser(username);
}
