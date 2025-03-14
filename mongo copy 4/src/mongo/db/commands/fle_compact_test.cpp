/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <MurmurHash3.h>
#include <map>

#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/commands/fle2_compact.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/fle_query_interface_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/shell/kms_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

const FLEUserKey& getUserKey() {
    static std::string userVec = hexblob::decode(
        "cbebdf05fe16099fef502a6d045c1cbb77d29d2fe19f51aec5079a81008305d8868358845d2e3ab38e4fa9cbffcd651a0fc07201d7c9ed9ca3279bfa7cd673ec37b362a0aaa92f95062405a999afd49e4b1f7f818f766c49715407011ac37fa9"_sd);
    static FLEUserKey userKey(KeyMaterial(userVec.begin(), userVec.end()));
    return userKey;
}

class TestKeyVault : public FLEKeyVault {
public:
    TestKeyVault() : _random(123456), _localKey(getLocalKey()) {}

    static SymmetricKey getLocalKey() {
        const uint8_t buf[]{0x32, 0x78, 0x34, 0x34, 0x2b, 0x78, 0x64, 0x75, 0x54, 0x61, 0x42, 0x42,
                            0x6b, 0x59, 0x31, 0x36, 0x45, 0x72, 0x35, 0x44, 0x75, 0x41, 0x44, 0x61,
                            0x67, 0x68, 0x76, 0x53, 0x34, 0x76, 0x77, 0x64, 0x6b, 0x67, 0x38, 0x74,
                            0x70, 0x50, 0x70, 0x33, 0x74, 0x7a, 0x36, 0x67, 0x56, 0x30, 0x31, 0x41,
                            0x31, 0x43, 0x77, 0x62, 0x44, 0x39, 0x69, 0x74, 0x51, 0x32, 0x48, 0x46,
                            0x44, 0x67, 0x50, 0x57, 0x4f, 0x70, 0x38, 0x65, 0x4d, 0x61, 0x43, 0x31,
                            0x4f, 0x69, 0x37, 0x36, 0x36, 0x4a, 0x7a, 0x58, 0x5a, 0x42, 0x64, 0x42,
                            0x64, 0x62, 0x64, 0x4d, 0x75, 0x72, 0x64, 0x6f, 0x6e, 0x4a, 0x31, 0x64};

        return SymmetricKey(&buf[0], sizeof(buf), 0, SymmetricKeyId("test"), 0);
    }

    KeyMaterial getKey(const UUID& uuid) override;
    BSONObj getEncryptedKey(const UUID& uuid) override;
    SymmetricKey& getKMSLocalKey() {
        return _localKey;
    }

    uint64_t getCount() const {
        return _dynamicKeys.size();
    }

private:
    PseudoRandom _random;
    stdx::unordered_map<UUID, KeyMaterial, UUID::Hash> _dynamicKeys;
    SymmetricKey _localKey;
};

KeyMaterial TestKeyVault::getKey(const UUID& uuid) {
    if (uuid == userKeyId) {
        return getUserKey().data;
    } else {
        if (_dynamicKeys.find(uuid) != _dynamicKeys.end()) {
            return _dynamicKeys[uuid];
        }

        std::vector<uint8_t> materialVector(96);
        _random.fill(&materialVector[0], materialVector.size());
        KeyMaterial material(materialVector.begin(), materialVector.end());
        _dynamicKeys.insert({uuid, material});
        return material;
    }
}

KeyStoreRecord makeKeyStoreRecord(UUID id, ConstDataRange cdr) {
    KeyStoreRecord ksr;
    ksr.set_id(id);
    auto now = Date_t::now();
    ksr.setCreationDate(now);
    ksr.setUpdateDate(now);
    ksr.setStatus(0);
    ksr.setKeyMaterial(cdr);

    LocalMasterKey mk;

    ksr.setMasterKey(mk.toBSON());
    return ksr;
}

BSONObj TestKeyVault::getEncryptedKey(const UUID& uuid) {
    auto dek = getKey(uuid);

    std::vector<std::uint8_t> ciphertext(crypto::aeadCipherOutputLength(dek->size()));

    uassertStatusOK(crypto::aeadEncryptLocalKMS(_localKey, *dek, {ciphertext}));

    return makeKeyStoreRecord(uuid, ciphertext).toBSON();
}

UUID fieldNameToUUID(StringData field) {
    std::array<uint8_t, UUID::kNumBytes> buf;

    MurmurHash3_x86_128(field.rawData(), field.size(), 123456, buf.data());
    return UUID::fromCDR(buf);
}

class FleCompactTest : public ServiceContextMongoDTest {
public:
    struct ESCTestTokens {
        ESCDerivedFromDataTokenAndContentionFactorToken contentionDerived;
        ESCTwiceDerivedTagToken twiceDerivedTag;
        ESCTwiceDerivedValueToken twiceDerivedValue;
    };
    struct ECCTestTokens {
        ECCDerivedFromDataTokenAndContentionFactorToken contentionDerived;
        ECCTwiceDerivedTagToken twiceDerivedTag;
        ECCTwiceDerivedValueToken twiceDerivedValue;
    };
    struct InsertionState {
        uint64_t count{0};
        uint64_t pos{0};
        uint64_t toInsertCount{0};
        std::vector<std::pair<uint64_t, uint64_t>> toDeleteRanges;
        std::map<uint64_t, int> insertedIds;
        std::string value;
    };

protected:
    void setUp();
    void tearDown();

    void createCollection(const NamespaceString& ns);

    void assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc);

    // TODO: SERVER-73303 delete when v2 is enabled by default
    void assertESCDocument(BSONObj obj, bool exists, uint64_t index, uint64_t count);
    // TODO: SERVER-73303 delete when v2 is enabled by default
    void assertESCNullDocument(BSONObj obj, bool exists, uint64_t position, uint64_t count);

    // TODO: SERVER-73303 delete when v2 is enabled by default
    void assertECCDocument(BSONObj obj, bool exists, uint64_t index, uint64_t start, uint64_t end);
    // TODO: SERVER-73303 delete when v2 is enabled by default
    void assertECCNullDocument(BSONObj obj, bool exists, uint64_t position);

    void assertESCNonAnchorDocument(BSONObj obj, bool exists, uint64_t cpos);
    void assertESCAnchorDocument(BSONObj obj, bool exists, uint64_t apos, uint64_t cpos);

    ESCTestTokens getTestESCTokens(BSONObj obj);

    // TODO: SERVER-73303 delete when v2 is enabled by default
    ECCTestTokens getTestECCTokens(BSONObj obj);

    // TODO: SERVER-73303 delete when v2 is enabled by default
    ECOCCompactionDocument generateTestECOCDocument(BSONObj obj);

    ECOCCompactionDocumentV2 generateTestECOCDocumentV2(BSONObj obj);

    EncryptedFieldConfig generateEncryptedFieldConfig(
        const std::set<std::string>& encryptedFieldNames);

    CompactStructuredEncryptionData generateCompactCommand(
        const std::set<std::string>& encryptedFieldNames);

    std::vector<char> generatePlaceholder(UUID keyId, BSONElement value);

    void doSingleInsert(int id, BSONObj encryptedFieldsObj);
    void doSingleDelete(int id, BSONObj encryptedFieldsObj);

    void insertFieldValues(StringData fieldName, std::map<std::string, InsertionState>& values);
    void deleteFieldValues(StringData fieldName, std::map<std::string, InsertionState>& values);

protected:
    ServiceContext::UniqueOperationContext _opCtx;

    repl::StorageInterface* _storage{nullptr};

    std::unique_ptr<FLEQueryInterfaceMock> _queryImpl;

    std::unique_ptr<repl::UnreplicatedWritesBlock> _uwb;

    TestKeyVault _keyVault;

    EncryptedStateCollectionsNamespaces _namespaces;
};

void FleCompactTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));
    repl::ReplicationCoordinator::get(service)
        ->setFollowerMode(repl::MemberState::RS_PRIMARY)
        .ignore();

    _opCtx = cc().makeOperationContext();
    _uwb = std::make_unique<repl::UnreplicatedWritesBlock>(_opCtx.get());

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    _storage = repl::StorageInterface::get(service);

    _queryImpl = std::make_unique<FLEQueryInterfaceMock>(_opCtx.get(), _storage);

    _namespaces.edcNss = NamespaceString::createNamespaceString_forTest("test.edc");
    _namespaces.escNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.esc");
    _namespaces.eccNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecc");
    _namespaces.ecocNss = NamespaceString::createNamespaceString_forTest("test.enxcol_.coll.ecoc");
    _namespaces.ecocRenameNss = NamespaceString::createNamespaceString_forTest("test.ecoc.compact");

    createCollection(_namespaces.edcNss);
    createCollection(_namespaces.escNss);
    createCollection(_namespaces.eccNss);
    createCollection(_namespaces.ecocNss);
}

void FleCompactTest::tearDown() {
    _uwb.reset(nullptr);
    _opCtx.reset(nullptr);
    ServiceContextMongoDTest::tearDown();
}

void FleCompactTest::createCollection(const NamespaceString& ns) {
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC = _storage->createCollection(
        _opCtx.get(),
        NamespaceString::createNamespaceString_forTest(ns.dbName(), ns.coll()),
        collectionOptions);
    ASSERT_OK(statusCC);
}

void FleCompactTest::assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc) {
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.edcNss), edc);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.escNss), esc);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.eccNss), ecc);
    ASSERT_EQ(_queryImpl->countDocuments(_namespaces.ecocNss), ecoc);
}

void FleCompactTest::assertESCDocument(BSONObj obj, bool exists, uint64_t index, uint64_t count) {
    auto tokens = getTestESCTokens(obj);
    auto doc = _queryImpl->getById(_namespaces.escNss,
                                   ESCCollection::generateId(tokens.twiceDerivedTag, index));

    ASSERT_EQ(doc.isEmpty(), !exists);

    if (exists) {
        auto escDoc =
            uassertStatusOK(ESCCollection::decryptDocument(tokens.twiceDerivedValue, doc));
        ASSERT_FALSE(escDoc.compactionPlaceholder);
        ASSERT_EQ(escDoc.position, 0);
        ASSERT_EQ(escDoc.count, count);
    }
}

void FleCompactTest::assertESCNullDocument(BSONObj obj, bool exists, uint64_t pos, uint64_t count) {
    auto tokens = getTestESCTokens(obj);
    auto doc = _queryImpl->getById(_namespaces.escNss,
                                   ESCCollection::generateId(tokens.twiceDerivedTag, boost::none));

    ASSERT_EQ(doc.isEmpty(), !exists);

    if (exists) {
        auto nullDoc =
            uassertStatusOK(ESCCollection::decryptNullDocument(tokens.twiceDerivedValue, doc));
        ASSERT_EQ(nullDoc.position, pos);
        ASSERT_EQ(nullDoc.count, count);
    }
}

void FleCompactTest::assertESCNonAnchorDocument(BSONObj obj, bool exists, uint64_t cpos) {
    auto tokens = getTestESCTokens(obj);
    auto doc = _queryImpl->getById(
        _namespaces.escNss, ESCCollection::generateNonAnchorId(tokens.twiceDerivedTag, cpos));
    ASSERT_EQ(doc.isEmpty(), !exists);
    ASSERT(!doc.hasField("value"_sd));
}

void FleCompactTest::assertESCAnchorDocument(BSONObj obj,
                                             bool exists,
                                             uint64_t apos,
                                             uint64_t cpos) {
    auto tokens = getTestESCTokens(obj);
    auto doc = _queryImpl->getById(_namespaces.escNss,
                                   ESCCollection::generateAnchorId(tokens.twiceDerivedTag, apos));
    ASSERT_EQ(doc.isEmpty(), !exists);

    if (exists) {
        auto anchorDoc =
            uassertStatusOK(ESCCollection::decryptAnchorDocument(tokens.twiceDerivedValue, doc));
        ASSERT_EQ(anchorDoc.position, 0);
        ASSERT_EQ(anchorDoc.count, cpos);
    }
}

void FleCompactTest::assertECCDocument(
    BSONObj obj, bool exists, uint64_t index, uint64_t start, uint64_t end) {
    auto tokens = getTestECCTokens(obj);
    auto doc = _queryImpl->getById(_namespaces.eccNss,
                                   ECCCollection::generateId(tokens.twiceDerivedTag, index));

    ASSERT_EQ(doc.isEmpty(), !exists);

    if (exists) {
        auto eccDoc =
            uassertStatusOK(ECCCollection::decryptDocument(tokens.twiceDerivedValue, doc));
        ASSERT(eccDoc.valueType == ECCValueType::kNormal);
        ASSERT_EQ(eccDoc.start, start);
        ASSERT_EQ(eccDoc.end, end);
    }
}

void FleCompactTest::assertECCNullDocument(BSONObj obj, bool exists, uint64_t pos) {
    auto tokens = getTestECCTokens(obj);
    auto doc = _queryImpl->getById(_namespaces.eccNss,
                                   ECCCollection::generateId(tokens.twiceDerivedTag, boost::none));

    ASSERT_EQ(doc.isEmpty(), !exists);

    if (exists) {
        auto nullDoc =
            uassertStatusOK(ECCCollection::decryptNullDocument(tokens.twiceDerivedValue, doc));
        ASSERT_EQ(nullDoc.position, pos);
    }
}

FleCompactTest::ESCTestTokens FleCompactTest::getTestESCTokens(BSONObj obj) {
    auto element = obj.firstElement();
    auto indexKeyId = fieldNameToUUID(element.fieldNameStringData());
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);
    auto eltCdr = ConstDataRange(element.value(), element.value() + element.valuesize());
    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, eltCdr);

    FleCompactTest::ESCTestTokens tokens;
    tokens.contentionDerived = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 0);
    tokens.twiceDerivedValue =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedValueToken(tokens.contentionDerived);
    tokens.twiceDerivedTag =
        FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(tokens.contentionDerived);
    return tokens;
}

FleCompactTest::ECCTestTokens FleCompactTest::getTestECCTokens(BSONObj obj) {
    auto element = obj.firstElement();
    auto indexKeyId = fieldNameToUUID(element.fieldNameStringData());
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(c1token);
    auto eltCdr = ConstDataRange(element.value(), element.value() + element.valuesize());
    auto eccDataToken =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, eltCdr);

    FleCompactTest::ECCTestTokens tokens;
    tokens.contentionDerived = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateECCDerivedFromDataTokenAndContentionFactorToken(eccDataToken, 0);
    tokens.twiceDerivedValue =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(tokens.contentionDerived);
    tokens.twiceDerivedTag =
        FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(tokens.contentionDerived);
    return tokens;
}

ECOCCompactionDocument FleCompactTest::generateTestECOCDocument(BSONObj obj) {
    ECOCCompactionDocument doc;
    doc.fieldName = obj.firstElementFieldName();
    doc.esc = getTestESCTokens(obj).contentionDerived;
    doc.ecc = getTestECCTokens(obj).contentionDerived;
    return doc;
}

ECOCCompactionDocumentV2 FleCompactTest::generateTestECOCDocumentV2(BSONObj obj) {
    ECOCCompactionDocumentV2 doc;
    doc.fieldName = obj.firstElementFieldName();
    doc.esc = getTestESCTokens(obj).contentionDerived;
    return doc;
}

EncryptedFieldConfig FleCompactTest::generateEncryptedFieldConfig(
    const std::set<std::string>& encryptedFieldNames) {
    BSONObjBuilder builder;
    EncryptedFieldConfig efc;
    std::vector<EncryptedField> encryptedFields;

    for (const auto& field : encryptedFieldNames) {
        EncryptedField ef;
        ef.setKeyId(fieldNameToUUID(field));
        ef.setPath(field);
        ef.setBsonType("string"_sd);
        QueryTypeConfig q(QueryTypeEnum::Equality);
        auto x = ef.getQueries();
        x = std::move(q);
        ef.setQueries(x);
        encryptedFields.push_back(std::move(ef));
    }

    efc.setEscCollection(_namespaces.escNss.coll());
    efc.setEccCollection(_namespaces.eccNss.coll());
    efc.setEcocCollection(_namespaces.ecocNss.coll());
    efc.setFields(std::move(encryptedFields));
    return efc;
}

CompactStructuredEncryptionData FleCompactTest::generateCompactCommand(
    const std::set<std::string>& encryptedFieldNames) {
    CompactStructuredEncryptionData cmd(_namespaces.edcNss);
    auto efc = generateEncryptedFieldConfig(encryptedFieldNames);
    auto compactionTokens = FLEClientCrypto::generateCompactionTokens(efc, &_keyVault);
    cmd.setCompactionTokens(compactionTokens);
    return cmd;
}

std::vector<char> FleCompactTest::generatePlaceholder(UUID keyId, BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(keyId);
    ep.setValue(value);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

void FleCompactTest::doSingleInsert(int id, BSONObj encryptedFieldsObj) {
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("plainText", "sample");

    for (auto&& elt : encryptedFieldsObj) {
        UUID uuid = fieldNameToUUID(elt.fieldNameStringData());
        auto buf = generatePlaceholder(uuid, elt);
        builder.appendBinData(
            elt.fieldNameStringData(), buf.size(), BinDataType::Encrypt, buf.data());
    }

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::transformPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc =
        generateEncryptedFieldConfig(encryptedFieldsObj.getFieldNames<std::set<std::string>>());

    int stmtId = 0;

    uassertStatusOK(processInsert(
        _queryImpl.get(), _namespaces.edcNss, serverPayload, efc, &stmtId, result, false));
}

void FleCompactTest::doSingleDelete(int id, BSONObj encryptedFieldsObj) {
    auto efc =
        generateEncryptedFieldConfig(encryptedFieldsObj.getFieldNames<std::set<std::string>>());

    auto doc = EncryptionInformationHelpers::encryptionInformationSerializeForDelete(
        _namespaces.edcNss, efc, &_keyVault);

    auto ei = EncryptionInformation::parse(IDLParserContext("test"), doc);

    write_ops::DeleteOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setMulti(false);

    write_ops::DeleteCommandRequest deleteRequest(_namespaces.edcNss);
    deleteRequest.setDeletes({entry});
    deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);

    std::unique_ptr<CollatorInterface> collator;
    auto expCtx = make_intrusive<ExpressionContext>(_opCtx.get(),
                                                    std::move(collator),
                                                    deleteRequest.getNamespace(),
                                                    deleteRequest.getLegacyRuntimeConstants(),
                                                    deleteRequest.getLet());

    processDelete(_queryImpl.get(), expCtx, deleteRequest);
}

void FleCompactTest::insertFieldValues(StringData field,
                                       std::map<std::string, InsertionState>& values) {
    static int insertId = 1;

    for (auto& [value, state] : values) {
        for (; state.toInsertCount > 0; state.toInsertCount--) {
            BSONObjBuilder builder;
            builder.append(field, value);
            doSingleInsert(insertId, builder.obj());
            ++state.pos;
            ++state.count;
            state.insertedIds[state.count] = insertId;
            ++insertId;
        }
    }
}

void FleCompactTest::deleteFieldValues(StringData field,
                                       std::map<std::string, InsertionState>& values) {
    for (auto& [value, state] : values) {
        BSONObjBuilder builder;
        builder.append(field, value);
        auto entry = builder.obj();

        for (auto& range : state.toDeleteRanges) {
            for (auto ctr = range.first; ctr <= range.second; ctr++) {
                if (state.insertedIds.find(ctr) == state.insertedIds.end()) {
                    continue;
                }
                doSingleDelete(state.insertedIds[ctr], entry);
                state.insertedIds.erase(ctr);
            }
        }
    }
}

TEST_F(FleCompactTest, GetUniqueECOCDocsFromEmptyECOC) {
    ECOCStats stats;
    std::set<std::string> fieldSet = {"first", "ssn"};
    auto cmd = generateCompactCommand(fieldSet);
    auto docsV1 = getUniqueCompactionDocuments(_queryImpl.get(), cmd, _namespaces.ecocNss, &stats);
    ASSERT(docsV1.empty());

    auto docs = getUniqueCompactionDocumentsV2(_queryImpl.get(), cmd, _namespaces.ecocNss, &stats);
    ASSERT(docs.empty());
}

TEST_F(FleCompactTest, GetUniqueECOCDocsMultipleFieldsWithManyDuplicateValues) {
    ECOCStats stats;
    std::set<std::string> fieldSet = {"first", "ssn", "city", "state", "zip"};
    int numInserted = 0;
    int uniqueValuesPerField = 10;
    stdx::unordered_set<ECOCCompactionDocument> expectedV1;
    stdx::unordered_set<ECOCCompactionDocumentV2> expected;

    for (auto& field : fieldSet) {
        std::map<std::string, InsertionState> values;
        for (int i = 1; i <= uniqueValuesPerField; i++) {
            auto val = "value_" + std::to_string(i);
            expectedV1.insert(generateTestECOCDocument(BSON(field << val)));
            expected.insert(generateTestECOCDocumentV2(BSON(field << val)));
            values[val].toInsertCount = i;
            numInserted += i;
        }
        insertFieldValues(field, values);
    }

    assertDocumentCounts(numInserted, numInserted, 0, numInserted);

    auto cmd = generateCompactCommand(fieldSet);
    auto docsV1 = getUniqueCompactionDocuments(_queryImpl.get(), cmd, _namespaces.ecocNss, &stats);
    ASSERT(docsV1 == expectedV1);
    auto docs = getUniqueCompactionDocumentsV2(_queryImpl.get(), cmd, _namespaces.ecocNss, &stats);
    ASSERT(docs == expected);
}

// TODO: SERVER-73303 delete when v2 is enabled by default
TEST_F(FleCompactTest, CompactValueWithNonExistentESCAndECCEntries) {
    ECStats escStats, eccStats;
    auto testPair = BSON("first"
                         << "brian");
    auto ecocDoc = generateTestECOCDocument(testPair);

    assertDocumentCounts(0, 0, 0, 0);

    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);

    // nothing should have changed in the state collections
    assertDocumentCounts(0, 0, 0, 0);
}

// TODO: SERVER-73303 delete when v2 is enabled by default
TEST_F(FleCompactTest, CompactValueWithESCEntriesExcludingNullDoc) {
    ECStats escStats, eccStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string val1 = "roger";
    const std::string val2 = "roderick";

    values[val1].toInsertCount = 15;
    values[val2].toInsertCount = 1;
    insertFieldValues(key, values);
    assertDocumentCounts(16, 16, 0, 16);

    // compact the value with multiple entries
    auto testPair = BSON(key << val1);
    auto ecocDoc = generateTestECOCDocument(testPair);

    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(16, 2, 0, 16);
    assertESCNullDocument(testPair, true, 15, 15);

    // compact the value with just a single entry
    testPair = BSON(key << val2);
    ecocDoc = generateTestECOCDocument(testPair);

    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(16, 2, 0, 16);
    assertESCNullDocument(testPair, true, 1, 1);
}

// TODO: SERVER-73303 delete when v2 is enabled by default
TEST_F(FleCompactTest, CompactValueWithESCEntriesIncludingNullDoc) {
    ECStats escStats, eccStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string val = "ruben";
    auto testPair = BSON(key << val);
    auto ecocDoc = generateTestECOCDocument(testPair);

    values[val].toInsertCount = 34;
    insertFieldValues(key, values);
    assertDocumentCounts(34, 34, 0, 34);

    // compact once to get the null doc
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(34, 1, 0, 34);
    assertESCNullDocument(testPair, true, 34, 34);

    // insert more values following the null doc
    values[val].toInsertCount = 16;
    insertFieldValues(key, values);
    assertDocumentCounts(50, 17, 0, 50);

    // compact again, but now with a null doc
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(50, 1, 0, 50);
    // null doc has pos value of 51 instead of 50 to account for the placeholder document
    assertESCNullDocument(testPair, true, 51, 50);

    // compact again, but now just the null doc
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(50, 1, 0, 50);
    // null doc's pos value bumped to 52 for the placeholder doc
    assertESCNullDocument(testPair, true, 52, 50);

    // insert one more, and compact
    values[val].toInsertCount = 1;
    insertFieldValues(key, values);
    assertDocumentCounts(51, 2, 0, 51);
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(51, 1, 0, 51);
    assertESCNullDocument(testPair, true, 54, 51);
}

// TODO: SERVER-73303 delete when v2 is enabled by default
TEST_F(FleCompactTest, CompactValueWithECCEntriesWithoutNullAndAllEntriesDeleted) {
    ECStats escStats, eccStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string val1 = "reginald";
    const std::string val2 = "rudolph";

    values[val1].toInsertCount = 1;
    values[val2].toInsertCount = 9;
    insertFieldValues(key, values);
    assertDocumentCounts(10, 10, 0, 10);

    // delete all entries
    values[val1].toDeleteRanges = {{1, 1}};
    values[val2].toDeleteRanges = {{1, 9}};
    deleteFieldValues(key, values);
    assertDocumentCounts(0, 10, 10, 20);

    // compact should still insert null docs in ESC for each value
    // compact only inserts a null doc in ECC if a merge squashes entries (i.e. only for val2)
    auto testPair = BSON(key << val1);
    auto ecocDoc = generateTestECOCDocument(testPair);

    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(0, 10, 10, 20);
    assertESCNullDocument(testPair, true, 1, 1);
    assertECCNullDocument(testPair, false, 0);
    assertECCDocument(testPair, true, 1, 1, 1);

    testPair = BSON(key << val2);
    ecocDoc = generateTestECOCDocument(testPair);
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(0, 2, 3, 20);
    assertESCNullDocument(testPair, true, 9, 9);
    assertECCNullDocument(testPair, true, 9);
    assertECCDocument(testPair, true, 11, 1, 9);
}

// TODO: SERVER-73303 delete when v2 is enabled by default
TEST_F(FleCompactTest, CompactValueWithECCEntriesWithNullAndAllEntriesDeleted) {
    ECStats escStats, eccStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string val = "silas";
    auto testPair = BSON(key << val);
    auto ecocDoc = generateTestECOCDocument(testPair);

    // insert entries 1 to 10
    values[val].toInsertCount = 10;
    insertFieldValues(key, values);
    assertDocumentCounts(10, 10, 0, 10);

    // delete entries 4, 5, and 1 (ECC inserts at pos 1 to 3)
    values[val].toDeleteRanges = {{4, 5}, {1, 1}};
    deleteFieldValues(key, values);
    assertDocumentCounts(7, 10, 3, 13);
    assertECCDocument(testPair, true, 1, 4, 4);
    assertECCDocument(testPair, true, 2, 5, 5);
    assertECCDocument(testPair, true, 3, 1, 1);

    // first compact puts placeholder at pos 4 & merges deletes to {{1,1}, {4,5}} at pos 5 & 6
    // null doc inserted with pos of 3
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(7, 1, 3, 13);
    assertESCNullDocument(testPair, true, 10, 10);
    assertECCNullDocument(testPair, true, 3);
    assertECCDocument(testPair, false, 4, 0, 0);
    assertECCDocument(testPair, true, 5, 1, 1);
    assertECCDocument(testPair, true, 6, 4, 5);

    // delete entries 8, 9, and 2 (ECC inserts at pos 7 to 9)
    values[val].toDeleteRanges = {{8, 9}, {2, 2}};
    deleteFieldValues(key, values);
    assertDocumentCounts(4, 1, 6, 16);
    assertECCDocument(testPair, true, 7, 8, 8);
    assertECCDocument(testPair, true, 8, 9, 9);
    assertECCDocument(testPair, true, 9, 2, 2);

    // second compact puts placeholder at pos 10 & merges deletions to {{1,2}, {4,5}, {8,9}}
    // at pos 11-13. Null doc updated with pos 9.
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(4, 1, 4, 16);
    assertECCNullDocument(testPair, true, 9);
    assertECCDocument(testPair, false, 10, 0, 0);
    assertECCDocument(testPair, true, 11, 1, 2);
    assertECCDocument(testPair, true, 12, 4, 5);
    assertECCDocument(testPair, true, 13, 8, 9);

    // delete the remaining entries (ECC inserts at pos 14-17)
    values[val].toDeleteRanges = {{3, 3}, {6, 7}, {10, 10}};
    deleteFieldValues(key, values);
    assertDocumentCounts(0, 1, 8, 20);
    assertECCDocument(testPair, true, 14, 3, 3);
    assertECCDocument(testPair, true, 15, 6, 6);
    assertECCDocument(testPair, true, 16, 7, 7);
    assertECCDocument(testPair, true, 17, 10, 10);

    // final compact squashes ECC entries (inserts placeholder at 18, merged doc at 19)
    // and updates null doc
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(0, 1, 2, 20);
    assertECCNullDocument(testPair, true, 17);
    assertECCDocument(testPair, false, 18, 0, 0);
    assertECCDocument(testPair, true, 19, 1, 10);
}

// TODO: SERVER-73303 delete when v2 is enabled by default
TEST_F(FleCompactTest, CompactValueWithECCEntriesThatAreNotMergeable) {
    ECStats escStats, eccStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string val = "samson";
    auto testPair = BSON(key << val);
    auto ecocDoc = generateTestECOCDocument(testPair);

    // insert entries 1 to 10
    values[val].toInsertCount = 10;
    insertFieldValues(key, values);
    assertDocumentCounts(10, 10, 0, 10);

    // delete odd numbered entries (ECC inserts at pos 1-5)
    values[val].toDeleteRanges = {{1, 1}, {3, 3}, {5, 5}, {7, 7}, {9, 9}};
    deleteFieldValues(key, values);
    assertDocumentCounts(5, 10, 5, 15);

    // compact is a no-op on ECC, hence no null doc was inserted
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(5, 1, 5, 15);
    assertECCNullDocument(testPair, false, 0);
    assertECCDocument(testPair, true, 1, 1, 1);
    assertECCDocument(testPair, true, 2, 3, 3);
    assertECCDocument(testPair, true, 3, 5, 5);
    assertECCDocument(testPair, true, 4, 7, 7);
    assertECCDocument(testPair, true, 5, 9, 9);

    // delete 2,4,6 & 8 (ECC inserts at pos 6-9)
    values[val].toDeleteRanges = {{2, 2}, {4, 4}, {6, 6}, {8, 8}};
    deleteFieldValues(key, values);
    assertDocumentCounts(1, 1, 9, 19);
    assertECCDocument(testPair, true, 6, 2, 2);
    assertECCDocument(testPair, true, 7, 4, 4);
    assertECCDocument(testPair, true, 8, 6, 6);
    assertECCDocument(testPair, true, 9, 8, 8);

    // compact puts placeholder at pos 10 & merges deletes to {{1,9}} at pos 11
    // null doc inserted with pos 9
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(1, 1, 2, 19);
    assertECCNullDocument(testPair, true, 9);
    assertECCDocument(testPair, true, 11, 1, 9);

    // running compact again is a no-op on ECC, so null doc is unchanged
    compactOneFieldValuePair(_queryImpl.get(), ecocDoc, _namespaces, &escStats, &eccStats);
    assertDocumentCounts(1, 1, 2, 19);
    assertECCNullDocument(testPair, true, 9);
    assertECCDocument(testPair, true, 11, 1, 9);
}

// Tests V2 compaction on an ESC that does not contain non-anchors
TEST_F(FleCompactTest, CompactValueV2_NoNonAnchors) {
    ECStats escStats;
    auto testPair = BSON("first"
                         << "brian");
    auto ecocDoc = generateTestECOCDocumentV2(testPair);
    assertDocumentCounts(0, 0, 0, 0);

    // Compact an empty ESC; assert compact inserts anchor at apos = 1 with cpos = 0
    // Note: this tests compact where EmuBinary returns (cpos = 0, apos = 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(0, 1, 0, 0);
    assertESCAnchorDocument(testPair, true, 1, 0);

    // Compact an ESC containing only non-null anchors
    // Note: this tests compact where EmuBinary returns (cpos = null, apos > 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(0, 1, 0, 0);
}

TEST_F(FleCompactTest, CompactValueV2_NoNullAnchors) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2ProtocolVersion2", true);
    ECStats escStats;
    std::map<std::string, InsertionState> values;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    auto testPair = BSON(key << value);
    auto ecocDoc = generateTestECOCDocumentV2(testPair);

    // Insert 15 of the same value; assert non-anchors 1 thru 15
    values[value].toInsertCount = 15;
    insertFieldValues(key, values);
    assertDocumentCounts(15, 15, 0, 15);
    for (uint64_t i = 1; i <= 15; i++) {
        assertESCNonAnchorDocument(testPair, true, i);
    }

    // Compact ESC which should only have non-anchors
    // Note: this tests compact where EmuBinary returns (cpos > 0, apos = 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(15, 16, 0, 15);
    assertESCAnchorDocument(testPair, true, 1, 15);

    // Compact ESC which should now have a fresh anchor and stale non-anchors
    // Note: this tests compact where EmuBinary returns (cpos = null, apos > 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(15, 16, 0, 15);

    // Insert another 15 of the same value; assert non-anchors 16 thru 30
    values[value].toInsertCount = 15;
    insertFieldValues(key, values);
    assertDocumentCounts(30, 31, 0, 30);
    for (uint64_t i = 16; i <= 30; i++) {
        assertESCNonAnchorDocument(testPair, true, i);
    }

    // Compact ESC which should now have fresh anchors and fresh non-anchors
    // Note: this tests compact where EmuBinary returns (cpos > 0, apos > 0)
    compactOneFieldValuePairV2(_queryImpl.get(), ecocDoc, _namespaces.escNss, &escStats);
    assertDocumentCounts(30, 32, 0, 30);
    assertESCAnchorDocument(testPair, true, 2, 30);
}

TEST_F(FleCompactTest, RandomESCNonAnchorDeletions) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2ProtocolVersion2", true);
    ECStats escStats;
    constexpr auto key = "first"_sd;
    const std::string value = "roger";
    std::map<std::string, InsertionState> values;
    size_t deleteCount = 150;
    size_t limit = deleteCount * sizeof(PrfBlock);

    // read from empty ESC; limit to 150 tags
    auto idSet = readRandomESCNonAnchorIds(_opCtx.get(), _namespaces.escNss, limit, &escStats);
    ASSERT(idSet.empty());
    ASSERT_EQ(escStats.getRead(), 0);

    // populate the ESC with 300 non-anchors
    values[value].toInsertCount = 300;
    insertFieldValues(key, values);
    assertDocumentCounts(300, 300, 0, 300);

    // read from non-empty ESC; limit to 0 tags
    idSet = readRandomESCNonAnchorIds(_opCtx.get(), _namespaces.escNss, 0, &escStats);
    ASSERT(idSet.empty());
    ASSERT_EQ(escStats.getRead(), 0);

    // read from non-empty ESC; limit to 150 tags
    idSet = readRandomESCNonAnchorIds(_opCtx.get(), _namespaces.escNss, limit, &escStats);
    ASSERT(!idSet.empty());
    ASSERT_EQ(idSet.size(), deleteCount);
    ASSERT_EQ(escStats.getRead(), deleteCount);

    // delete the tags from the ESC; 30 tags at a time
    cleanupESCNonAnchors(_opCtx.get(), _namespaces.escNss, idSet, 30, &escStats);
    ASSERT_EQ(escStats.getDeleted(), deleteCount);
    assertDocumentCounts(300, 300 - deleteCount, 0, 300);

    // assert the deletes are scattered
    // (ie. less than 150 deleted in first half of the original set of 300)
    auto tokens = getTestESCTokens(BSON(key << value));
    int counter = 0;
    for (uint64_t i = 1; i <= deleteCount; i++) {
        auto doc = _queryImpl->getById(
            _namespaces.escNss, ESCCollection::generateNonAnchorId(tokens.twiceDerivedTag, i));
        if (!doc.isEmpty()) {
            counter++;
        }
    }
    ASSERT_LT(counter, deleteCount);
}

}  // namespace
}  // namespace mongo
