/*
 *     Copyright 2016 Couchbase, Inc
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

#include "storeddockey.h"
#include <mcbp/protocol/unsigned_leb128.h>
#include <iomanip>

StoredDocKey::StoredDocKey(const DocKey& key) {
    if (key.getEncoding() == DocKeyEncodesCollectionId::Yes) {
        keydata.resize(key.size());
        std::copy(key.data(), key.data() + key.size(), keydata.begin());
    } else {
        // 1 byte for the Default CollectionID
        keydata.resize(key.size() + 1);
        keydata[0] = DefaultCollectionLeb128Encoded;
        std::copy(key.data(), key.data() + key.size(), keydata.begin() + 1);
    }
}

StoredDocKey::StoredDocKey(const std::string& key, CollectionID cid) {
    cb::mcbp::unsigned_leb128<CollectionIDType> leb128(cid);
    keydata.resize(key.size() + leb128.size());
    std::copy(key.begin(),
              key.end(),
              std::copy(leb128.begin(), leb128.end(), keydata.begin()));
}

StoredDocKey::StoredDocKey(const DocKey& key, CollectionID cid) {
    cb::mcbp::unsigned_leb128<CollectionIDType> leb128(cid);
    keydata.resize(key.size() + leb128.size());
    std::copy(key.begin(),
              key.end(),
              std::copy(leb128.begin(), leb128.end(), keydata.begin()));
}

CollectionID StoredDocKey::getCollectionID() const {
    return cb::mcbp::decode_unsigned_leb128<CollectionIDType>({data(), size()})
            .first;
}

DocKey StoredDocKey::makeDocKeyWithoutCollectionID() const {
    auto decoded = cb::mcbp::decode_unsigned_leb128<CollectionIDType>(
            {data(), size()});
    return {decoded.second.data(),
            decoded.second.size(),
            DocKeyEncodesCollectionId::No};
}

std::string StoredDocKey::to_string() const {
    return DocKey(*this).to_string();
}

const char* StoredDocKey::c_str() const {
    // Locate the leb128 stop byte, and return pointer after that
    auto key = cb::mcbp::skip_unsigned_leb128<CollectionIDType>(
            {reinterpret_cast<const uint8_t*>(keydata.data()), keydata.size()});

    if (!key.empty()) {
        return &keydata.c_str()[keydata.size() - key.size()];
    }
    return nullptr;
}

std::ostream& operator<<(std::ostream& os, const StoredDocKey& key) {
    return os << key.to_string();
}

CollectionID SerialisedDocKey::getCollectionID() const {
    return cb::mcbp::decode_unsigned_leb128<CollectionIDType>({bytes, length})
            .first;
}

bool SerialisedDocKey::operator==(const DocKey& rhs) const {
    auto rhsIdAndData = rhs.getIdAndKey();
    auto lhsIdAndData = cb::mcbp::decode_unsigned_leb128<CollectionIDType>(
            {data(), size()});
    return lhsIdAndData.first == rhsIdAndData.first &&
           lhsIdAndData.second.size() == rhsIdAndData.second.size() &&
           std::equal(lhsIdAndData.second.begin(),
                      lhsIdAndData.second.end(),
                      rhsIdAndData.second.begin());
}

SerialisedDocKey::SerialisedDocKey(cb::const_byte_buffer key,
                                   CollectionID cid) {
    cb::mcbp::unsigned_leb128<CollectionIDType> leb128(cid);
    length = gsl::narrow_cast<uint8_t>(key.size() + leb128.size());
    std::copy(key.begin(),
              key.end(),
              std::copy(leb128.begin(), leb128.end(), bytes));
}

std::ostream& operator<<(std::ostream& os, const SerialisedDocKey& key) {
    auto leb128 = cb::mcbp::decode_unsigned_leb128<CollectionIDType>(
            {reinterpret_cast<const uint8_t*>(key.data()), key.size()});
    os << "cid:0x" << std::hex << leb128.first << ":"
       << std::string(reinterpret_cast<const char*>(leb128.second.data()),
                      leb128.second.size());
    os << ", size:" << key.size();
    return os;
}
