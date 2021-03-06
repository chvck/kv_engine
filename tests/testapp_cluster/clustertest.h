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

#pragma once

#include <folly/portability/GTest.h>
#include <memcached/dockey.h>
#include <memcached/vbucket.h>

class MemcachedConnection;

namespace cb {
namespace test {

class Bucket;
class Cluster;

/**
 * Base class to build clustered tests. The test suite starts up a
 * cluster with 4 nodes, and creates a bucket named default with 8 vbuckets
 * and 2 replicas and 67MB memory size
 */
class ClusterTest : public ::testing::Test {
public:
    // Per-test-case set-up.
    // Called before the first test in this test case.
    static void SetUpTestCase();

    // Per-test-case tear-down.
    // Called after the last test in this test case.
    static void TearDownTestCase();

    // Create a bucket named default
    static void createDefaultBucket();

protected:
    // per test setup function.
    void SetUp() override;

    // per test tear-down function.
    void TearDown() override;

    void getReplica(MemcachedConnection& conn,
                    Vbid vbid,
                    const std::string& key);

    /**
     * Create a collection enabled key
     *
     * @param cid The collection identifier
     * @param key The key to encode
     * @return a key which may be used on a collection enabled connection
     */
    static std::string createKey(CollectionIDType cid, const std::string& key);

    /// An enum containing the various collections defined in the default
    /// bucket by default by the test framework
    enum class Collection : CollectionIDType {
        Default = 0,
        Fruit = 8,
        Vegetable = 9
    };

    /// Wrapper method to allow us to use the names of the collections
    /// rather the numeric values
    static std::string createKey(Collection cid, const std::string& key) {
        return createKey(CollectionIDType(cid), key);
    }

    static std::unique_ptr<Cluster> cluster;
};

class UpgradeTest : public ClusterTest {
public:
    static void SetUpTestCase();

    static void TearDownTestCase() {
        bucket.reset();
        ClusterTest::TearDownTestCase();
    }

protected:
    static std::shared_ptr<Bucket> bucket;
};

} // namespace test
} // namespace cb
