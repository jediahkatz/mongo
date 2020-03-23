/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include <math.h>

#include "mongo/platform/basic.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/logv2/log.h"

namespace DocumentTests {

using std::numeric_limits;
using std::string;
using std::vector;

mongo::Document::FieldPair getNthField(mongo::Document doc, size_t index) {
    mongo::FieldIterator it(doc);
    while (index--)  // advance index times
        it.next();
    return it.next();
}

namespace Document {

using mongo::Document;

BSONObj toBson(const Document& document) {
    return document.toBson();
}

Document fromBson(BSONObj obj) {
    return Document(obj);
}

void assertRoundTrips(const Document& document1) {
    BSONObj obj1 = toBson(document1);
    Document document2 = fromBson(obj1);
    BSONObj obj2 = toBson(document2);
    ASSERT_BSONOBJ_EQ(obj1, obj2);
    ASSERT_DOCUMENT_EQ(document1, document2);
}

TEST(DocumentConstruction, Default) {
    Document document;
    ASSERT_EQUALS(0ULL, document.computeSize());
    assertRoundTrips(document);
}

TEST(DocumentConstruction, FromEmptyBson) {
    Document document = fromBson(BSONObj());
    ASSERT_EQUALS(0ULL, document.computeSize());
    assertRoundTrips(document);
}

TEST(DocumentConstruction, FromNonEmptyBson) {
    Document document = fromBson(BSON("a" << 1 << "b"
                                          << "q"));
    ASSERT_EQUALS(2ULL, document.computeSize());
    ASSERT_EQUALS("a", getNthField(document, 0).first.toString());
    ASSERT_EQUALS(1, getNthField(document, 0).second.getInt());
    ASSERT_EQUALS("b", getNthField(document, 1).first.toString());
    ASSERT_EQUALS("q", getNthField(document, 1).second.getString());
}

TEST(DocumentConstruction, FromInitializerList) {
    auto document = Document{{"a", 1}, {"b", "q"_sd}};
    ASSERT_EQUALS(2ULL, document.computeSize());
    ASSERT_EQUALS("a", getNthField(document, 0).first.toString());
    ASSERT_EQUALS(1, getNthField(document, 0).second.getInt());
    ASSERT_EQUALS("b", getNthField(document, 1).first.toString());
    ASSERT_EQUALS("q", getNthField(document, 1).second.getString());
}

TEST(DocumentConstruction, FromEmptyDocumentClone) {
    Document document;
    ASSERT_EQUALS(0ULL, document.computeSize());
    // Prior to SERVER-26462, cloning an empty document would cause a segmentation fault.
    Document documentClone = document.clone();
    ASSERT_DOCUMENT_EQ(document, documentClone);

    // Prior to SERVER-39209 this would make ASAN complain.
    Document documentClone2 = documentClone.clone();
    ASSERT_DOCUMENT_EQ(document, documentClone2);

    // For good measure, try a third clone
    Document documentClone3 = documentClone2.clone();
    ASSERT_DOCUMENT_EQ(document, documentClone3);
}

TEST(DocumentConstruction, FromBsonReset) {
    auto document = Document{{"a", 1}, {"b", "q"_sd}};
    auto bson = toBson(document);

    MutableDocument md;
    md.reset(bson, false);
    auto newDocument = md.freeze();

    ASSERT_BSONOBJ_EQ(bson, toBson(newDocument));
}

/**
 * Appends to 'builder' an object nested 'depth' levels deep.
 */
void appendNestedObject(size_t depth, BSONObjBuilder* builder) {
    if (depth == 1U) {
        builder->append("a", 1);
    } else {
        BSONObjBuilder subobj(builder->subobjStart("a"));
        appendNestedObject(depth - 1, &subobj);
        subobj.doneFast();
    }
}

TEST(DocumentSerialization, CanSerializeDocumentExactlyAtDepthLimit) {
    BSONObjBuilder builder;
    appendNestedObject(BSONDepth::getMaxAllowableDepth(), &builder);
    BSONObj originalBSONObj = builder.obj();

    Document doc(originalBSONObj);
    BSONObjBuilder serializationResult;
    doc.toBson(&serializationResult);
    ASSERT_BSONOBJ_EQ(originalBSONObj, serializationResult.obj());
}

TEST(DocumentSerialization, CannotSerializeDocumentThatExceedsDepthLimit) {
    MutableDocument md;
    md.addField("a", Value(1));
    Document doc(md.freeze());
    for (size_t idx = 0; idx < BSONDepth::getMaxAllowableDepth(); ++idx) {
        MutableDocument md;
        md.addField("nested", Value(doc));
        doc = md.freeze();
    }

    BSONObjBuilder throwaway;
    ASSERT_THROWS_CODE(doc.toBson(&throwaway), AssertionException, ErrorCodes::Overflow);
    throwaway.abandon();
}

/** Add Document fields. */
class AddField {
public:
    void run() {
        MutableDocument md;
        md.addField("foo", Value(1));
        ASSERT_EQUALS(1ULL, md.peek().computeSize());
        ASSERT_EQUALS(1, md.peek()["foo"].getInt());
        md.addField("bar", Value(99));
        ASSERT_EQUALS(2ULL, md.peek().computeSize());
        ASSERT_EQUALS(99, md.peek()["bar"].getInt());
        // No assertion is triggered by a duplicate field name.
        md.addField("a", Value(5));

        Document final = md.freeze();
        ASSERT_EQUALS(3ULL, final.computeSize());
        assertRoundTrips(final);
    }
};

/** Get Document values. */
class GetValue {
public:
    void run() {
        Document document = fromBson(BSON("a" << 1 << "b" << 2.2));
        ASSERT_EQUALS(1, document["a"].getInt());
        ASSERT_EQUALS(1, document["a"].getInt());
        ASSERT_EQUALS(2.2, document["b"].getDouble());
        ASSERT_EQUALS(2.2, document["b"].getDouble());
        // Missing field.
        ASSERT(document["c"].missing());
        ASSERT(document["c"].missing());
        assertRoundTrips(document);
    }
};

/** Get Document fields. */
class SetField {
public:
    void run() {
        Document original = fromBson(BSON("a" << 1 << "b" << 2.2 << "c" << 99));

        // Initial positions. Used at end of function to make sure nothing moved
        const Position apos = original.positionOf("a");
        const Position bpos = original.positionOf("c");
        const Position cpos = original.positionOf("c");

        MutableDocument md(original);

        // Set the first field.
        md.setField("a", Value("foo"_sd));
        ASSERT_EQUALS(3ULL, md.peek().computeSize());
        ASSERT_EQUALS("foo", md.peek()["a"].getString());
        ASSERT_EQUALS("foo", getNthField(md.peek(), 0).second.getString());
        assertRoundTrips(md.peek());
        // Set the second field.
        md["b"] = Value("bar"_sd);
        ASSERT_EQUALS(3ULL, md.peek().computeSize());
        ASSERT_EQUALS("bar", md.peek()["b"].getString());
        ASSERT_EQUALS("bar", getNthField(md.peek(), 1).second.getString());
        assertRoundTrips(md.peek());

        // Remove the second field.
        md.setField("b", Value());
        LOGV2(20585, "{md_peek}", "md_peek"_attr = md.peek().toString());
        ASSERT_EQUALS(2ULL, md.peek().computeSize());
        ASSERT(md.peek()["b"].missing());
        ASSERT_EQUALS("a", getNthField(md.peek(), 0).first.toString());
        ASSERT_EQUALS("c", getNthField(md.peek(), 1).first.toString());
        ASSERT_EQUALS(99, md.peek()["c"].getInt());
        assertRoundTrips(md.peek());

        // Remove the first field.
        md["a"] = Value();
        ASSERT_EQUALS(1ULL, md.peek().computeSize());
        ASSERT(md.peek()["a"].missing());
        ASSERT_EQUALS("c", getNthField(md.peek(), 0).first.toString());
        ASSERT_EQUALS(99, md.peek()["c"].getInt());
        assertRoundTrips(md.peek());

        // Remove the final field. Verify document is empty.
        md.remove("c");
        ASSERT(md.peek().empty());
        ASSERT_EQUALS(0ULL, md.peek().computeSize());
        ASSERT_DOCUMENT_EQ(md.peek(), Document());
        ASSERT(!FieldIterator(md.peek()).more());
        ASSERT(md.peek()["c"].missing());
        assertRoundTrips(md.peek());

        // Set a nested field using []
        md["x"]["y"]["z"] = Value("nested"_sd);
        ASSERT_VALUE_EQ(md.peek()["x"]["y"]["z"], Value("nested"_sd));

        // Set a nested field using setNestedField
        FieldPath xxyyzz("xx.yy.zz");
        md.setNestedField(xxyyzz, Value("nested"_sd));
        ASSERT_VALUE_EQ(md.peek().getNestedField(xxyyzz), Value("nested"_sd));

        // Set a nested fields through an existing empty document
        md["xxx"] = Value(Document());
        md["xxx"]["yyy"] = Value(Document());
        FieldPath xxxyyyzzz("xxx.yyy.zzz");
        md.setNestedField(xxxyyyzzz, Value("nested"_sd));
        ASSERT_VALUE_EQ(md.peek().getNestedField(xxxyyyzzz), Value("nested"_sd));

        // Make sure nothing moved
        ASSERT_EQUALS(apos, md.peek().positionOf("a"));
        ASSERT_EQUALS(bpos, md.peek().positionOf("c"));
        ASSERT_EQUALS(cpos, md.peek().positionOf("c"));
        ASSERT_EQUALS(Position(), md.peek().positionOf("d"));
    }
};

/** Document comparator. */
class Compare {
public:
    void run() {
        assertComparison(0, BSONObj(), BSONObj());
        assertComparison(0, BSON("a" << 1), BSON("a" << 1));
        assertComparison(-1, BSONObj(), BSON("a" << 1));
        assertComparison(-1, BSON("a" << 1), BSON("c" << 1));
        assertComparison(0, BSON("a" << 1 << "r" << 2), BSON("a" << 1 << "r" << 2));
        assertComparison(-1, BSON("a" << 1), BSON("a" << 1 << "r" << 2));
        assertComparison(0, BSON("a" << 2), BSON("a" << 2));
        assertComparison(-1, BSON("a" << 1), BSON("a" << 2));
        assertComparison(-1, BSON("a" << 1 << "b" << 1), BSON("a" << 1 << "b" << 2));
        // numbers sort before strings
        assertComparison(-1,
                         BSON("a" << 1),
                         BSON("a"
                              << "foo"));
        // numbers sort before strings, even if keys compare otherwise
        assertComparison(-1,
                         BSON("b" << 1),
                         BSON("a"
                              << "foo"));
        // null before number, even if keys compare otherwise
        assertComparison(-1, BSON("z" << BSONNULL), BSON("a" << 1));
    }

public:
    int cmp(const BSONObj& a, const BSONObj& b) {
        int result = DocumentComparator().compare(fromBson(a), fromBson(b));
        return  // sign
            result < 0 ? -1 : result > 0 ? 1 : 0;
    }
    void assertComparison(int expectedResult, const BSONObj& a, const BSONObj& b) {
        ASSERT_EQUALS(expectedResult, cmp(a, b));
        ASSERT_EQUALS(-expectedResult, cmp(b, a));
        if (expectedResult == 0) {
            ASSERT_EQUALS(hash(a), hash(b));
        }
    }
    size_t hash(const BSONObj& obj) {
        size_t seed = 0x106e1e1;
        const StringData::ComparatorInterface* stringComparator = nullptr;
        Document(obj).hash_combine(seed, stringComparator);
        return seed;
    }
};

/** Shallow copy clone of a single field Document. */
class Clone {
public:
    void run() {
        const Document document = fromBson(BSON("a" << BSON("b" << 1)));
        MutableDocument cloneOnDemand(document);

        // Check equality.
        ASSERT_DOCUMENT_EQ(document, cloneOnDemand.peek());
        // Check pointer equality of sub document.
        ASSERT_EQUALS(document["a"].getDocument().getPtr(),
                      cloneOnDemand.peek()["a"].getDocument().getPtr());


        // Change field in clone and ensure the original document's field is unchanged.
        cloneOnDemand.setField(StringData("a"), Value(2));
        ASSERT_VALUE_EQ(Value(1), document.getNestedField(FieldPath("a.b")));


        // setNestedField and ensure the original document is unchanged.

        cloneOnDemand.reset(document);
        vector<Position> path;
        ASSERT_VALUE_EQ(Value(1), document.getNestedField(FieldPath("a.b"), &path));

        cloneOnDemand.setNestedField(path, Value(2));

        ASSERT_VALUE_EQ(Value(1), document.getNestedField(FieldPath("a.b")));
        ASSERT_VALUE_EQ(Value(2), cloneOnDemand.peek().getNestedField(FieldPath("a.b")));
        ASSERT_DOCUMENT_EQ(DOC("a" << DOC("b" << 1)), document);
        ASSERT_DOCUMENT_EQ(DOC("a" << DOC("b" << 2)), cloneOnDemand.freeze());
    }
};

/** Shallow copy clone of a multi field Document. */
class CloneMultipleFields {
public:
    void run() {
        Document document = fromBson(fromjson("{a:1,b:['ra',4],c:{z:1},d:'lal'}"));
        Document clonedDocument = document.clone();
        ASSERT_DOCUMENT_EQ(document, clonedDocument);
    }
};

/** FieldIterator for an empty Document. */
class FieldIteratorEmpty {
public:
    void run() {
        FieldIterator iterator((Document()));
        ASSERT(!iterator.more());
    }
};

/** FieldIterator for a single field Document. */
class FieldIteratorSingle {
public:
    void run() {
        FieldIterator iterator(fromBson(BSON("a" << 1)));
        ASSERT(iterator.more());
        Document::FieldPair field = iterator.next();
        ASSERT_EQUALS("a", field.first.toString());
        ASSERT_EQUALS(1, field.second.getInt());
        ASSERT(!iterator.more());
    }
};

/** FieldIterator for a multiple field Document. */
class FieldIteratorMultiple {
public:
    void run() {
        FieldIterator iterator(fromBson(BSON("a" << 1 << "b" << 5.6 << "c"
                                                 << "z")));
        ASSERT(iterator.more());
        Document::FieldPair field = iterator.next();
        ASSERT_EQUALS("a", field.first.toString());
        ASSERT_EQUALS(1, field.second.getInt());
        ASSERT(iterator.more());

        Document::FieldPair field2 = iterator.next();
        ASSERT_EQUALS("b", field2.first.toString());
        ASSERT_EQUALS(5.6, field2.second.getDouble());
        ASSERT(iterator.more());

        Document::FieldPair field3 = iterator.next();
        ASSERT_EQUALS("c", field3.first.toString());
        ASSERT_EQUALS("z", field3.second.getString());
        ASSERT(!iterator.more());
    }
};

class AllTypesDoc {
public:
    void run() {
        // These are listed in order of BSONType with some duplicates
        append("minkey", MINKEY);
        // EOO not valid in middle of BSONObj
        append("double", 1.0);
        append("c++", "string\0after NUL"_sd);
        append("StringData", "string\0after NUL"_sd);
        append("emptyObj", BSONObj());
        append("filledObj", BSON("a" << 1));
        append("emptyArray", BSON("" << BSONArray()).firstElement());
        append("filledArray", BSON("" << BSON_ARRAY(1 << "a")).firstElement());
        append("binData", BSONBinData("a\0b", 3, BinDataGeneral));
        append("binDataCustom", BSONBinData("a\0b", 3, bdtCustom));
        append("binDataUUID", BSONBinData("123456789\0abcdef", 16, bdtUUID));
        append("undefined", BSONUndefined);
        append("oid", OID());
        append("true", true);
        append("false", false);
        append("date", jsTime());
        append("null", BSONNULL);
        append("regex", BSONRegEx(".*"));
        append("regexFlags", BSONRegEx(".*", "i"));
        append("regexEmpty", BSONRegEx("", ""));
        append("dbref", BSONDBRef("foo", OID()));
        append("code", BSONCode("function() {}"));
        append("codeNul", BSONCode("var nul = '\0'"_sd));
        append("symbol", BSONSymbol("foo"));
        append("symbolNul", BSONSymbol("f\0o"_sd));
        append("codeWScope", BSONCodeWScope("asdf", BSONObj()));
        append("codeWScopeWScope", BSONCodeWScope("asdf", BSON("one" << 1)));
        append("int", 1);
        append("timestamp", Timestamp());
        append("long", 1LL);
        append("very long", 1LL << 40);
        append("maxkey", MAXKEY);

        const BSONArray arr = arrBuilder.arr();

        // can't use append any more since arrBuilder is done
        objBuilder << "mega array" << arr;
        docBuilder["mega array"] = mongo::Value(values);

        const BSONObj obj = objBuilder.obj();
        const Document doc = docBuilder.freeze();

        const BSONObj obj2 = toBson(doc);
        const Document doc2 = fromBson(obj);

        // logical equality
        ASSERT_BSONOBJ_EQ(obj, obj2);
        ASSERT_DOCUMENT_EQ(doc, doc2);

        // binary equality
        ASSERT_EQUALS(obj.objsize(), obj2.objsize());
        ASSERT_EQUALS(memcmp(obj.objdata(), obj2.objdata(), obj.objsize()), 0);

        // ensure sorter serialization round-trips correctly
        BufBuilder bb;
        doc.serializeForSorter(bb);
        BufReader reader(bb.buf(), bb.len());
        const Document doc3 =
            Document::deserializeForSorter(reader, Document::SorterDeserializeSettings());
        BSONObj obj3 = toBson(doc3);
        ASSERT_EQUALS(obj.objsize(), obj3.objsize());
        ASSERT_EQUALS(memcmp(obj.objdata(), obj3.objdata(), obj.objsize()), 0);
    }

    template <typename T>
    void append(const char* name, const T& thing) {
        objBuilder << name << thing;
        arrBuilder << thing;
        docBuilder[name] = mongo::Value(thing);
        values.push_back(mongo::Value(thing));
    }

    vector<mongo::Value> values;
    MutableDocument docBuilder;
    BSONObjBuilder objBuilder;
    BSONArrayBuilder arrBuilder;
};
}  // namespace Document

namespace MetaFields {
using mongo::Document;
TEST(MetaFields, TextScoreBasics) {
    // Documents should not have a text score until it is set.
    ASSERT_FALSE(Document().metadata().hasTextScore());

    // Setting the text score should work as expected.
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(1.0);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasTextScore());
    ASSERT_EQ(1.0, doc.metadata().getTextScore());
}

TEST(MetaFields, RandValBasics) {
    // Documents should not have a random value until it is set.
    ASSERT_FALSE(Document().metadata().hasRandVal());

    // Setting the random value field should work as expected.
    MutableDocument docBuilder;
    docBuilder.metadata().setRandVal(1.0);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasRandVal());
    ASSERT_EQ(1, doc.metadata().getRandVal());

    // Setting the random value twice should keep the second value.
    MutableDocument docBuilder2;
    docBuilder2.metadata().setRandVal(1.0);
    docBuilder2.metadata().setRandVal(2.0);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasRandVal());
    ASSERT_EQ(2.0, doc2.metadata().getRandVal());
}

TEST(MetaFields, SearchScoreBasic) {
    // Documents should not have a search score until it is set.
    ASSERT_FALSE(Document().metadata().hasSearchScore());

    // Setting the search score field should work as expected.
    MutableDocument docBuilder;
    docBuilder.metadata().setSearchScore(1.23);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasSearchScore());
    ASSERT_EQ(1.23, doc.metadata().getSearchScore());

    // Setting the searchScore twice should keep the second value.
    MutableDocument docBuilder2;
    docBuilder2.metadata().setSearchScore(1.0);
    docBuilder2.metadata().setSearchScore(2.0);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasSearchScore());
    ASSERT_EQ(2.0, doc2.metadata().getSearchScore());
}

TEST(MetaFields, SearchHighlightsBasic) {
    // Documents should not have a search highlights until it is set.
    ASSERT_FALSE(Document().metadata().hasSearchHighlights());

    // Setting the search highlights field should work as expected.
    MutableDocument docBuilder;
    Value highlights = DOC_ARRAY("a"_sd
                                 << "b"_sd);
    docBuilder.metadata().setSearchHighlights(highlights);
    Document doc = docBuilder.freeze();
    ASSERT_TRUE(doc.metadata().hasSearchHighlights());
    ASSERT_VALUE_EQ(doc.metadata().getSearchHighlights(), highlights);

    // Setting the searchHighlights twice should keep the second value.
    MutableDocument docBuilder2;
    Value otherHighlights = DOC_ARRAY("snippet1"_sd
                                      << "snippet2"_sd
                                      << "snippet3"_sd);
    docBuilder2.metadata().setSearchHighlights(highlights);
    docBuilder2.metadata().setSearchHighlights(otherHighlights);
    Document doc2 = docBuilder2.freeze();
    ASSERT_TRUE(doc2.metadata().hasSearchHighlights());
    ASSERT_VALUE_EQ(doc2.metadata().getSearchHighlights(), otherHighlights);
}

TEST(MetaFields, IndexKeyMetadataSerializesCorrectly) {
    Document doc{BSON("a" << 1)};
    MutableDocument mutableDoc{doc};
    mutableDoc.metadata().setIndexKey(BSON("b" << 1));
    doc = mutableDoc.freeze();

    ASSERT_TRUE(doc.metadata().hasIndexKey());
    ASSERT_BSONOBJ_EQ(doc.metadata().getIndexKey(), BSON("b" << 1));

    auto serialized = doc.toBsonWithMetaData();
    ASSERT_BSONOBJ_EQ(serialized, BSON("a" << 1 << "$indexKey" << BSON("b" << 1)));
}

TEST(MetaFields, FromBsonWithMetadataAcceptsIndexKeyMetadata) {
    auto doc = Document::fromBsonWithMetaData(BSON("a" << 1 << "$indexKey" << BSON("b" << 1)));
    ASSERT_TRUE(doc.metadata().hasIndexKey());
    ASSERT_BSONOBJ_EQ(doc.metadata().getIndexKey(), BSON("b" << 1));
    auto bsonWithoutMetadata = doc.toBson();
    ASSERT_BSONOBJ_EQ(bsonWithoutMetadata, BSON("a" << 1));
}

TEST(MetaFields, CopyMetadataFromCopiesAllMetadata) {
    Document source = Document::fromBsonWithMetaData(
        BSON("a" << 1 << "$textScore" << 9.9 << "b" << 1 << "$randVal" << 42.0 << "c" << 1
                 << "$sortKey" << BSON("x" << 1) << "d" << 1 << "$dis" << 3.2 << "e" << 1 << "$pt"
                 << BSON_ARRAY(1 << 2) << "f" << 1 << "$searchScore" << 5.4 << "g" << 1
                 << "$searchHighlights"
                 << "foo"
                 << "h" << 1 << "$indexKey" << BSON("y" << 1)));

    MutableDocument destination{};
    destination.copyMetaDataFrom(source);
    auto result = destination.freeze();

    ASSERT_EQ(result.metadata().getTextScore(), 9.9);
    ASSERT_EQ(result.metadata().getRandVal(), 42.0);
    ASSERT_VALUE_EQ(result.metadata().getSortKey(), Value(1));
    ASSERT_EQ(result.metadata().getGeoNearDistance(), 3.2);
    ASSERT_VALUE_EQ(result.metadata().getGeoNearPoint(), Value{BSON_ARRAY(1 << 2)});
    ASSERT_EQ(result.metadata().getSearchScore(), 5.4);
    ASSERT_VALUE_EQ(result.metadata().getSearchHighlights(), Value{"foo"_sd});
    ASSERT_BSONOBJ_EQ(result.metadata().getIndexKey(), BSON("y" << 1));
}

class SerializationTest : public unittest::Test {
protected:
    Document roundTrip(const Document& input) {
        BufBuilder bb;
        input.serializeForSorter(bb);
        BufReader reader(bb.buf(), bb.len());
        return Document::deserializeForSorter(reader, Document::SorterDeserializeSettings());
    }

    void assertRoundTrips(const Document& input) {
        // Round trip to/from a buffer.
        auto output = roundTrip(input);
        ASSERT_DOCUMENT_EQ(output, input);
        ASSERT_EQ(output.metadata().hasTextScore(), input.metadata().hasTextScore());
        ASSERT_EQ(output.metadata().hasRandVal(), input.metadata().hasRandVal());
        ASSERT_EQ(output.metadata().hasSearchScore(), input.metadata().hasSearchScore());
        ASSERT_EQ(output.metadata().hasSearchHighlights(), input.metadata().hasSearchHighlights());
        ASSERT_EQ(output.metadata().hasIndexKey(), input.metadata().hasIndexKey());
        if (input.metadata().hasTextScore()) {
            ASSERT_EQ(output.metadata().getTextScore(), input.metadata().getTextScore());
        }
        if (input.metadata().hasRandVal()) {
            ASSERT_EQ(output.metadata().getRandVal(), input.metadata().getRandVal());
        }
        if (input.metadata().hasSearchScore()) {
            ASSERT_EQ(output.metadata().getSearchScore(), input.metadata().getSearchScore());
        }
        if (input.metadata().hasSearchHighlights()) {
            ASSERT_VALUE_EQ(output.metadata().getSearchHighlights(),
                            input.metadata().getSearchHighlights());
        }
        if (input.metadata().hasIndexKey()) {
            ASSERT_BSONOBJ_EQ(output.metadata().getIndexKey(), input.metadata().getIndexKey());
        }

        ASSERT(output.toBson().binaryEqual(input.toBson()));
    }
};

TEST_F(SerializationTest, MetaSerializationNoVals) {
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd
                                                        << "def"_sd));
    assertRoundTrips(docBuilder.freeze());
}

TEST_F(SerializationTest, MetaSerializationWithVals) {
    // Same as above test, but add a non-meta field as well.
    MutableDocument docBuilder(DOC("foo" << 10));
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd
                                                        << "def"_sd));
    docBuilder.metadata().setIndexKey(BSON("key" << 42));
    assertRoundTrips(docBuilder.freeze());
}

TEST_F(SerializationTest, MetaSerializationSearchHighlightsNonArray) {
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    // Everything should still round trip even if the searchHighlights metadata isn't an array.
    docBuilder.metadata().setSearchHighlights(Value(1.23));
    assertRoundTrips(docBuilder.freeze());
}

TEST(MetaFields, ToAndFromBson) {
    MutableDocument docBuilder;
    docBuilder.metadata().setTextScore(10.0);
    docBuilder.metadata().setRandVal(20.0);
    docBuilder.metadata().setSearchScore(30.0);
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd
                                                        << "def"_sd));
    Document doc = docBuilder.freeze();
    BSONObj obj = doc.toBsonWithMetaData();
    ASSERT_EQ(10.0, obj[Document::metaFieldTextScore].Double());
    ASSERT_EQ(20, obj[Document::metaFieldRandVal].numberLong());
    ASSERT_EQ(30.0, obj[Document::metaFieldSearchScore].Double());
    ASSERT_BSONOBJ_EQ(obj[Document::metaFieldSearchHighlights].embeddedObject(),
                      BSON_ARRAY("abc"_sd
                                 << "def"_sd));
    Document fromBson = Document::fromBsonWithMetaData(obj);
    ASSERT_TRUE(fromBson.metadata().hasTextScore());
    ASSERT_TRUE(fromBson.metadata().hasRandVal());
    ASSERT_EQ(10.0, fromBson.metadata().getTextScore());
    ASSERT_EQ(20, fromBson.metadata().getRandVal());
}

TEST(MetaFields, MetaFieldsIncludedInDocumentApproximateSize) {
    MutableDocument docBuilder;
    docBuilder.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd
                                                        << "def"_sd));
    const size_t smallMetadataDocSize = docBuilder.freeze().getApproximateSize();

    // The second document has a larger "search highlights" object.
    MutableDocument docBuilder2;
    docBuilder2.metadata().setSearchHighlights(DOC_ARRAY("abc"_sd
                                                         << "def"_sd
                                                         << "ghijklmnop"_sd));
    Document doc2 = docBuilder2.freeze();
    const size_t bigMetadataDocSize = doc2.getApproximateSize();
    ASSERT_GT(bigMetadataDocSize, smallMetadataDocSize);

    // Do a sanity check on the amount of space taken by metadata in document 2.
    ASSERT_LT(doc2.getMetadataApproximateSize(), 250U);

    Document emptyDoc;
    ASSERT_LT(emptyDoc.getMetadataApproximateSize(), 100U);
}

TEST(MetaFields, BadSerialization) {
    // Write an unrecognized option to the buffer.
    BufBuilder bb;
    // Signal there are 0 fields.
    bb.appendNum(0);
    // This would specify a meta field with an invalid type.
    bb.appendNum(char(DocumentMetadataFields::MetaType::kNumFields) + 1);
    // Signals end of input.
    bb.appendNum(char(0));
    BufReader reader(bb.buf(), bb.len());
    ASSERT_THROWS_CODE(
        Document::deserializeForSorter(reader, Document::SorterDeserializeSettings()),
        AssertionException,
        28744);
}
}  // namespace MetaFields

namespace Value {

using mongo::Value;

BSONObj toBson(const Value& value) {
    if (value.missing())
        return BSONObj();  // EOO

    BSONObjBuilder bob;
    value.addToBsonObj(&bob, "");
    return bob.obj();
}

Value fromBson(const BSONObj& obj) {
    BSONElement element = obj.firstElement();
    return Value(element);
}

void assertRoundTrips(const Value& value1) {
    BSONObj obj1 = toBson(value1);
    Value value2 = fromBson(obj1);
    BSONObj obj2 = toBson(value2);
    ASSERT_BSONOBJ_EQ(obj1, obj2);
    ASSERT_VALUE_EQ(value1, value2);
    ASSERT_EQUALS(value1.getType(), value2.getType());
}

class BSONArrayTest {
public:
    void run() {
        ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), DOC_ARRAY(1 << 2 << 3));
        ASSERT_VALUE_EQ(Value(BSONArray()), Value(vector<Value>()));
    }
};

/** Int type. */
class Int {
public:
    void run() {
        Value value = Value(5);
        ASSERT_EQUALS(5, value.getInt());
        ASSERT_EQUALS(5, value.getLong());
        ASSERT_EQUALS(5, value.getDouble());
        ASSERT_EQUALS(NumberInt, value.getType());
        assertRoundTrips(value);
    }
};

/** Long type. */
class Long {
public:
    void run() {
        Value value = Value(99LL);
        ASSERT_EQUALS(99, value.getLong());
        ASSERT_EQUALS(99, value.getDouble());
        ASSERT_EQUALS(NumberLong, value.getType());
        assertRoundTrips(value);
    }
};

/** Double type. */
class Double {
public:
    void run() {
        Value value = Value(5.5);
        ASSERT_EQUALS(5.5, value.getDouble());
        ASSERT_EQUALS(NumberDouble, value.getType());
        assertRoundTrips(value);
    }
};

/** String type. */
class String {
public:
    void run() {
        Value value = Value("foo"_sd);
        ASSERT_EQUALS("foo", value.getString());
        ASSERT_EQUALS(mongo::String, value.getType());
        assertRoundTrips(value);
    }
};

/** String with a null character. */
class StringWithNull {
public:
    void run() {
        string withNull("a\0b", 3);
        BSONObj objWithNull = BSON("" << withNull);
        ASSERT_EQUALS(withNull, objWithNull[""].str());
        Value value = fromBson(objWithNull);
        ASSERT_EQUALS(withNull, value.getString());
        assertRoundTrips(value);
    }
};

/**
 * SERVER-43205: Constructing a Value with a very large BSONElement string causes the Value
 * constructor to throw before it can completely initialize its ValueStorage member, which has the
 * potential to lead to incorrect state.
 */
class LongString {
public:
    void run() {
        std::string longString(16793500, 'x');
        auto obj = BSON("str" << longString);
        ASSERT_THROWS_CODE([&]() { Value{obj["str"]}; }(), DBException, 16493);
    }
};

/** Date type. */
class Date {
public:
    void run() {
        Value value = Value(Date_t::fromMillisSinceEpoch(999));
        ASSERT_EQUALS(999, value.getDate().toMillisSinceEpoch());
        ASSERT_EQUALS(mongo::Date, value.getType());
        assertRoundTrips(value);
    }
};

/** Timestamp type. */
class JSTimestamp {
public:
    void run() {
        Value value = Value(Timestamp(777));
        ASSERT(Timestamp(777) == value.getTimestamp());
        ASSERT_EQUALS(mongo::bsonTimestamp, value.getType());
        assertRoundTrips(value);

        value = Value(Timestamp(~0U, 3));
        ASSERT(Timestamp(~0U, 3) == value.getTimestamp());
        ASSERT_EQUALS(mongo::bsonTimestamp, value.getType());
        assertRoundTrips(value);
    }
};

/** Document with no fields. */
class EmptyDocument {
public:
    void run() {
        mongo::Document document = mongo::Document();
        Value value = Value(document);
        ASSERT_EQUALS(document.getPtr(), value.getDocument().getPtr());
        ASSERT_EQUALS(Object, value.getType());
        assertRoundTrips(value);
    }
};

/** Document type. */
class Document {
public:
    void run() {
        mongo::MutableDocument md;
        md.addField("a", Value(5));
        md.addField("apple", Value("rrr"_sd));
        md.addField("banana", Value(-.3));
        mongo::Document document = md.freeze();

        Value value = Value(document);
        // Check document pointers are equal.
        ASSERT_EQUALS(document.getPtr(), value.getDocument().getPtr());
        // Check document contents.
        ASSERT_EQUALS(5, document["a"].getInt());
        ASSERT_EQUALS("rrr", document["apple"].getString());
        ASSERT_EQUALS(-.3, document["banana"].getDouble());
        ASSERT_EQUALS(Object, value.getType());
        assertRoundTrips(value);
    }
};

/** Array with no elements. */
class EmptyArray {
public:
    void run() {
        vector<Value> array;
        Value value(array);
        const vector<Value>& array2 = value.getArray();

        ASSERT(array2.empty());
        ASSERT_EQUALS(Array, value.getType());
        ASSERT_EQUALS(0U, value.getArrayLength());
        assertRoundTrips(value);
    }
};

/** Array type. */
class Array {
public:
    void run() {
        vector<Value> array;
        array.push_back(Value(5));
        array.push_back(Value("lala"_sd));
        array.push_back(Value(3.14));
        Value value = Value(array);
        const vector<Value>& array2 = value.getArray();

        ASSERT(!array2.empty());
        ASSERT_EQUALS(array2.size(), 3U);
        ASSERT_EQUALS(5, array2[0].getInt());
        ASSERT_EQUALS("lala", array2[1].getString());
        ASSERT_EQUALS(3.14, array2[2].getDouble());
        ASSERT_EQUALS(mongo::Array, value.getType());
        ASSERT_EQUALS(3U, value.getArrayLength());
        assertRoundTrips(value);
    }
};

/** Oid type. */
class Oid {
public:
    void run() {
        Value value = fromBson(BSON("" << OID("abcdefabcdefabcdefabcdef")));
        ASSERT_EQUALS(OID("abcdefabcdefabcdefabcdef"), value.getOid());
        ASSERT_EQUALS(jstOID, value.getType());
        assertRoundTrips(value);
    }
};

/** Bool type. */
class Bool {
public:
    void run() {
        Value value = fromBson(BSON("" << true));
        ASSERT_EQUALS(true, value.getBool());
        ASSERT_EQUALS(mongo::Bool, value.getType());
        assertRoundTrips(value);
    }
};

/** Regex type. */
class Regex {
public:
    void run() {
        Value value = fromBson(fromjson("{'':/abc/}"));
        ASSERT_EQUALS(string("abc"), value.getRegex());
        ASSERT_EQUALS(RegEx, value.getType());
        assertRoundTrips(value);
    }
};

/** Symbol type (currently unsupported). */
class Symbol {
public:
    void run() {
        Value value(BSONSymbol("FOOBAR"));
        ASSERT_EQUALS("FOOBAR", value.getSymbol());
        ASSERT_EQUALS(mongo::Symbol, value.getType());
        assertRoundTrips(value);
    }
};

/** Undefined type. */
class Undefined {
public:
    void run() {
        Value value = Value(BSONUndefined);
        ASSERT_EQUALS(mongo::Undefined, value.getType());
        assertRoundTrips(value);
    }
};

/** Null type. */
class Null {
public:
    void run() {
        Value value = Value(BSONNULL);
        ASSERT_EQUALS(jstNULL, value.getType());
        assertRoundTrips(value);
    }
};

/** True value. */
class True {
public:
    void run() {
        Value value = Value(true);
        ASSERT_EQUALS(true, value.getBool());
        ASSERT_EQUALS(mongo::Bool, value.getType());
        assertRoundTrips(value);
    }
};

/** False value. */
class False {
public:
    void run() {
        Value value = Value(false);
        ASSERT_EQUALS(false, value.getBool());
        ASSERT_EQUALS(mongo::Bool, value.getType());
        assertRoundTrips(value);
    }
};

/** -1 value. */
class MinusOne {
public:
    void run() {
        Value value = Value(-1);
        ASSERT_EQUALS(-1, value.getInt());
        ASSERT_EQUALS(NumberInt, value.getType());
        assertRoundTrips(value);
    }
};

/** 0 value. */
class Zero {
public:
    void run() {
        Value value = Value(0);
        ASSERT_EQUALS(0, value.getInt());
        ASSERT_EQUALS(NumberInt, value.getType());
        assertRoundTrips(value);
    }
};

/** 1 value. */
class One {
public:
    void run() {
        Value value = Value(1);
        ASSERT_EQUALS(1, value.getInt());
        ASSERT_EQUALS(NumberInt, value.getType());
        assertRoundTrips(value);
    }
};

namespace Coerce {

class ToBoolBase {
public:
    virtual ~ToBoolBase() {}
    void run() {
        ASSERT_EQUALS(expected(), value().coerceToBool());
    }

protected:
    virtual Value value() = 0;
    virtual bool expected() = 0;
};

class ToBoolTrue : public ToBoolBase {
    bool expected() {
        return true;
    }
};

class ToBoolFalse : public ToBoolBase {
    bool expected() {
        return false;
    }
};

/** Coerce 0 to bool. */
class ZeroIntToBool : public ToBoolFalse {
    Value value() {
        return Value(0);
    }
};

/** Coerce -1 to bool. */
class NonZeroIntToBool : public ToBoolTrue {
    Value value() {
        return Value(-1);
    }
};

/** Coerce 0LL to bool. */
class ZeroLongToBool : public ToBoolFalse {
    Value value() {
        return Value(0LL);
    }
};

/** Coerce 5LL to bool. */
class NonZeroLongToBool : public ToBoolTrue {
    Value value() {
        return Value(5LL);
    }
};

/** Coerce 0.0 to bool. */
class ZeroDoubleToBool : public ToBoolFalse {
    Value value() {
        return Value(0);
    }
};

/** Coerce -1.3 to bool. */
class NonZeroDoubleToBool : public ToBoolTrue {
    Value value() {
        return Value(-1.3);
    }
};

/** Coerce "" to bool. */
class StringToBool : public ToBoolTrue {
    Value value() {
        return Value(StringData());
    }
};

/** Coerce {} to bool. */
class ObjectToBool : public ToBoolTrue {
    Value value() {
        return Value(mongo::Document());
    }
};

/** Coerce [] to bool. */
class ArrayToBool : public ToBoolTrue {
    Value value() {
        return Value(vector<Value>());
    }
};

/** Coerce Date(0) to bool. */
class DateToBool : public ToBoolTrue {
    Value value() {
        return Value(Date_t{});
    }
};

/** Coerce js literal regex to bool. */
class RegexToBool : public ToBoolTrue {
    Value value() {
        return fromBson(fromjson("{''://}"));
    }
};

/** Coerce true to bool. */
class TrueToBool : public ToBoolTrue {
    Value value() {
        return fromBson(BSON("" << true));
    }
};

/** Coerce false to bool. */
class FalseToBool : public ToBoolFalse {
    Value value() {
        return fromBson(BSON("" << false));
    }
};

/** Coerce null to bool. */
class NullToBool : public ToBoolFalse {
    Value value() {
        return Value(BSONNULL);
    }
};

/** Coerce undefined to bool. */
class UndefinedToBool : public ToBoolFalse {
    Value value() {
        return Value(BSONUndefined);
    }
};

class ToIntBase {
public:
    virtual ~ToIntBase() {}
    void run() {
        if (asserts())
            ASSERT_THROWS(value().coerceToInt(), AssertionException);
        else
            ASSERT_EQUALS(expected(), value().coerceToInt());
    }

protected:
    virtual Value value() = 0;
    virtual int expected() {
        return 0;
    }
    virtual bool asserts() {
        return false;
    }
};

/** Coerce -5 to int. */
class IntToInt : public ToIntBase {
    Value value() {
        return Value(-5);
    }
    int expected() {
        return -5;
    }
};

/** Coerce long to int. */
class LongToInt : public ToIntBase {
    Value value() {
        return Value(0xff00000007LL);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce 9.8 to int. */
class DoubleToInt : public ToIntBase {
    Value value() {
        return Value(9.8);
    }
    int expected() {
        return 9;
    }
};

/** Coerce null to int. */
class NullToInt : public ToIntBase {
    Value value() {
        return Value(BSONNULL);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce undefined to int. */
class UndefinedToInt : public ToIntBase {
    Value value() {
        return Value(BSONUndefined);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce "" to int unsupported. */
class StringToInt {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToInt(), AssertionException);
    }
};

/** Coerce maxInt to int */
class MaxIntToInt : public ToIntBase {
    Value value() {
        return Value((double)std::numeric_limits<int>::max());
    }
    int expected() {
        return std::numeric_limits<int>::max();
    }
};

/** Coerce minInt to int */
class MinIntToInt : public ToIntBase {
    Value value() {
        return Value((double)std::numeric_limits<int>::min());
    }
    int expected() {
        return std::numeric_limits<int>::min();
    }
};

/** Coerce maxInt + 1 to int */
class TooLargeToInt : public ToIntBase {
    Value value() {
        return Value((double)std::numeric_limits<int>::max() + 1);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce minInt - 1 to int */
class TooLargeNegativeToInt : public ToIntBase {
    Value value() {
        return Value((double)std::numeric_limits<int>::min() - 1);
    }
    bool asserts() {
        return true;
    }
};

class ToLongBase {
public:
    virtual ~ToLongBase() {}
    void run() {
        if (asserts())
            ASSERT_THROWS(value().coerceToLong(), AssertionException);
        else
            ASSERT_EQUALS(expected(), value().coerceToLong());
    }

protected:
    virtual Value value() = 0;
    virtual long long expected() {
        return 0;
    }
    virtual bool asserts() {
        return false;
    }
};

/** Coerce -5 to long. */
class IntToLong : public ToLongBase {
    Value value() {
        return Value(-5);
    }
    long long expected() {
        return -5;
    }
};

/** Coerce long to long. */
class LongToLong : public ToLongBase {
    Value value() {
        return Value(0xff00000007LL);
    }
    long long expected() {
        return 0xff00000007LL;
    }
};

/** Coerce 9.8 to long. */
class DoubleToLong : public ToLongBase {
    Value value() {
        return Value(9.8);
    }
    long long expected() {
        return 9;
    }
};

/** Coerce infinity to long. */
class InfToLong : public ToLongBase {
    Value value() {
        return Value(std::numeric_limits<double>::infinity());
    }
    bool asserts() {
        return true;
    }
};

/** Coerce negative infinity to long. **/
class NegInfToLong : public ToLongBase {
    Value value() {
        return Value(std::numeric_limits<double>::infinity() * -1);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce large to long. **/
class InvalidLargeToLong : public ToLongBase {
    Value value() {
        return Value(pow(2, 63));
    }
    bool asserts() {
        return true;
    }
};

/** Coerce lowest double to long. **/
class LowestDoubleToLong : public ToLongBase {
    Value value() {
        return Value(static_cast<double>(std::numeric_limits<long long>::lowest()));
    }
    long long expected() {
        return std::numeric_limits<long long>::lowest();
    }
};

/** Coerce 'towards infinity' to long **/
class TowardsInfinityToLong : public ToLongBase {
    Value value() {
        return Value(static_cast<double>(std::nextafter(std::numeric_limits<long long>::lowest(),
                                                        std::numeric_limits<double>::lowest())));
    }
    bool asserts() {
        return true;
    }
};

/** Coerce null to long. */
class NullToLong : public ToLongBase {
    Value value() {
        return Value(BSONNULL);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce undefined to long. */
class UndefinedToLong : public ToLongBase {
    Value value() {
        return Value(BSONUndefined);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce string to long unsupported. */
class StringToLong {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToLong(), AssertionException);
    }
};

class ToDoubleBase {
public:
    virtual ~ToDoubleBase() {}
    void run() {
        if (asserts())
            ASSERT_THROWS(value().coerceToDouble(), AssertionException);
        else
            ASSERT_EQUALS(expected(), value().coerceToDouble());
    }

protected:
    virtual Value value() = 0;
    virtual double expected() {
        return 0;
    }
    virtual bool asserts() {
        return false;
    }
};

/** Coerce -5 to double. */
class IntToDouble : public ToDoubleBase {
    Value value() {
        return Value(-5);
    }
    double expected() {
        return -5;
    }
};

/** Coerce long to double. */
class LongToDouble : public ToDoubleBase {
    Value value() {
        // A long that cannot be exactly represented as a double.
        return Value(static_cast<double>(0x8fffffffffffffffLL));
    }
    double expected() {
        return static_cast<double>(0x8fffffffffffffffLL);
    }
};

/** Coerce double to double. */
class DoubleToDouble : public ToDoubleBase {
    Value value() {
        return Value(9.8);
    }
    double expected() {
        return 9.8;
    }
};

/** Coerce null to double. */
class NullToDouble : public ToDoubleBase {
    Value value() {
        return Value(BSONNULL);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce undefined to double. */
class UndefinedToDouble : public ToDoubleBase {
    Value value() {
        return Value(BSONUndefined);
    }
    bool asserts() {
        return true;
    }
};

/** Coerce string to double unsupported. */
class StringToDouble {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToDouble(), AssertionException);
    }
};

class ToDateBase {
public:
    virtual ~ToDateBase() {}
    void run() {
        ASSERT_EQUALS(Date_t::fromMillisSinceEpoch(expected()), value().coerceToDate());
    }

protected:
    virtual Value value() = 0;
    virtual long long expected() = 0;
};

/** Coerce date to date. */
class DateToDate : public ToDateBase {
    Value value() {
        return Value(Date_t::fromMillisSinceEpoch(888));
    }
    long long expected() {
        return 888;
    }
};

/**
 * Convert timestamp to date.  This extracts the time portion of the timestamp, which
 * is different from BSON behavior of interpreting all bytes as a date.
 */
class TimestampToDate : public ToDateBase {
    Value value() {
        return Value(Timestamp(777, 666));
    }
    long long expected() {
        return 777 * 1000;
    }
};

/** Coerce string to date unsupported. */
class StringToDate {
public:
    void run() {
        ASSERT_THROWS(Value(StringData()).coerceToDate(), AssertionException);
    }
};

class ToStringBase {
public:
    virtual ~ToStringBase() {}
    void run() {
        ASSERT_EQUALS(expected(), value().coerceToString());
    }

protected:
    virtual Value value() = 0;
    virtual string expected() {
        return "";
    }
};

/** Coerce -0.2 to string. */
class DoubleToString : public ToStringBase {
    Value value() {
        return Value(-0.2);
    }
    string expected() {
        return "-0.2";
    }
};

/** Coerce -4 to string. */
class IntToString : public ToStringBase {
    Value value() {
        return Value(-4);
    }
    string expected() {
        return "-4";
    }
};

/** Coerce 10000LL to string. */
class LongToString : public ToStringBase {
    Value value() {
        return Value(10000LL);
    }
    string expected() {
        return "10000";
    }
};

/** Coerce string to string. */
class StringToString : public ToStringBase {
    Value value() {
        return Value("fO_o"_sd);
    }
    string expected() {
        return "fO_o";
    }
};

/** Coerce timestamp to string. */
class TimestampToString : public ToStringBase {
    Value value() {
        return Value(Timestamp(1, 2));
    }
    string expected() {
        return Timestamp(1, 2).toStringPretty();
    }
};

/** Coerce date to string. */
class DateToString : public ToStringBase {
    Value value() {
        return Value(Date_t::fromMillisSinceEpoch(1234567890123LL));
    }
    string expected() {
        return "2009-02-13T23:31:30.123Z";
    }  // from js
};

/** Coerce null to string. */
class NullToString : public ToStringBase {
    Value value() {
        return Value(BSONNULL);
    }
};

/** Coerce undefined to string. */
class UndefinedToString : public ToStringBase {
    Value value() {
        return Value(BSONUndefined);
    }
};

/** Coerce document to string unsupported. */
class DocumentToString {
public:
    void run() {
        ASSERT_THROWS(Value(mongo::Document()).coerceToString(), AssertionException);
    }
};

/** Coerce timestamp to timestamp. */
class TimestampToTimestamp {
public:
    void run() {
        Value value = Value(Timestamp(1010));
        ASSERT(Timestamp(1010) == value.coerceToTimestamp());
    }
};

/** Coerce date to timestamp unsupported. */
class DateToTimestamp {
public:
    void run() {
        ASSERT_THROWS(Value(Date_t::fromMillisSinceEpoch(1010)).coerceToTimestamp(),
                      AssertionException);
    }
};

}  // namespace Coerce

/** Get the "widest" of two numeric types. */
class GetWidestNumeric {
public:
    void run() {
        using mongo::Undefined;

        // Numeric types.
        assertWidest(NumberInt, NumberInt, NumberInt);
        assertWidest(NumberLong, NumberInt, NumberLong);
        assertWidest(NumberDouble, NumberInt, NumberDouble);
        assertWidest(NumberLong, NumberLong, NumberLong);
        assertWidest(NumberDouble, NumberLong, NumberDouble);
        assertWidest(NumberDouble, NumberDouble, NumberDouble);

        // Missing value and numeric types (result Undefined).
        assertWidest(Undefined, NumberInt, Undefined);
        assertWidest(Undefined, NumberInt, Undefined);
        assertWidest(Undefined, NumberLong, jstNULL);
        assertWidest(Undefined, NumberLong, Undefined);
        assertWidest(Undefined, NumberDouble, jstNULL);
        assertWidest(Undefined, NumberDouble, Undefined);

        // Missing value types (result Undefined).
        assertWidest(Undefined, jstNULL, jstNULL);
        assertWidest(Undefined, jstNULL, Undefined);
        assertWidest(Undefined, Undefined, Undefined);

        // Other types (result Undefined).
        assertWidest(Undefined, NumberInt, mongo::Bool);
        assertWidest(Undefined, mongo::String, NumberDouble);
    }

private:
    void assertWidest(BSONType expectedWidest, BSONType a, BSONType b) {
        ASSERT_EQUALS(expectedWidest, Value::getWidestNumeric(a, b));
        ASSERT_EQUALS(expectedWidest, Value::getWidestNumeric(b, a));
    }
};

/** Add a Value to a BSONObj. */
class AddToBsonObj {
public:
    void run() {
        BSONObjBuilder bob;
        Value(4.4).addToBsonObj(&bob, "a");
        Value(22).addToBsonObj(&bob, "b");
        Value("astring"_sd).addToBsonObj(&bob, "c");
        ASSERT_BSONOBJ_EQ(BSON("a" << 4.4 << "b" << 22 << "c"
                                   << "astring"),
                          bob.obj());
    }
};

/** Add a Value to a BSONArray. */
class AddToBsonArray {
public:
    void run() {
        BSONArrayBuilder bab;
        Value(4.4).addToBsonArray(&bab);
        Value(22).addToBsonArray(&bab);
        Value("astring"_sd).addToBsonArray(&bab);
        ASSERT_BSONOBJ_EQ(BSON_ARRAY(4.4 << 22 << "astring"), bab.arr());
    }
};

/** Value comparator. */
class Compare {
public:
    void run() {
        BSONObjBuilder undefinedBuilder;
        undefinedBuilder.appendUndefined("");
        BSONObj undefined = undefinedBuilder.obj();

        // Undefined / null.
        assertComparison(0, undefined, undefined);
        assertComparison(-1, undefined, BSON("" << BSONNULL));
        assertComparison(0, BSON("" << BSONNULL), BSON("" << BSONNULL));

        // Undefined / null with other types.
        assertComparison(-1, undefined, BSON("" << 1));
        assertComparison(-1,
                         undefined,
                         BSON(""
                              << "bar"));
        assertComparison(-1, BSON("" << BSONNULL), BSON("" << -1));
        assertComparison(-1,
                         BSON("" << BSONNULL),
                         BSON(""
                              << "bar"));

        // Numeric types.
        assertComparison(0, 5, 5LL);
        assertComparison(0, -2, -2.0);
        assertComparison(0, 90LL, 90.0);
        assertComparison(-1, 5, 6LL);
        assertComparison(-1, -2, 2.1);
        assertComparison(1, 90LL, 89.999);
        assertComparison(-1, 90, 90.1);
        assertComparison(
            0, numeric_limits<double>::quiet_NaN(), numeric_limits<double>::signaling_NaN());
        assertComparison(-1, numeric_limits<double>::quiet_NaN(), 5);

        // strings compare between numbers and objects
        assertComparison(1, "abc", 90);
        assertComparison(-1,
                         "abc",
                         BSON("a"
                              << "b"));

        // String comparison.
        assertComparison(-1, "", "a");
        assertComparison(0, "a", "a");
        assertComparison(-1, "a", "b");
        assertComparison(-1, "aa", "b");
        assertComparison(1, "bb", "b");
        assertComparison(1, "bb", "b");
        assertComparison(1, "b-", "b");
        assertComparison(-1, "b-", "ba");
        // With a null character.
        assertComparison(1, string("a\0", 2), "a");

        // Object.
        assertComparison(0, fromjson("{'':{}}"), fromjson("{'':{}}"));
        assertComparison(0, fromjson("{'':{x:1}}"), fromjson("{'':{x:1}}"));
        assertComparison(-1, fromjson("{'':{}}"), fromjson("{'':{x:1}}"));
        assertComparison(-1, fromjson("{'':{'z': 1}}"), fromjson("{'':{'a': 'a'}}"));

        // Array.
        assertComparison(0, fromjson("{'':[]}"), fromjson("{'':[]}"));
        assertComparison(-1, fromjson("{'':[0]}"), fromjson("{'':[1]}"));
        assertComparison(-1, fromjson("{'':[0,0]}"), fromjson("{'':[1]}"));
        assertComparison(-1, fromjson("{'':[0]}"), fromjson("{'':[0,0]}"));
        assertComparison(-1, fromjson("{'':[0]}"), fromjson("{'':['']}"));

        // OID.
        assertComparison(0, OID("abcdefabcdefabcdefabcdef"), OID("abcdefabcdefabcdefabcdef"));
        assertComparison(1, OID("abcdefabcdefabcdefabcdef"), OID("010101010101010101010101"));

        // Bool.
        assertComparison(0, true, true);
        assertComparison(0, false, false);
        assertComparison(1, true, false);

        // Date.
        assertComparison(0, Date_t::fromMillisSinceEpoch(555), Date_t::fromMillisSinceEpoch(555));
        assertComparison(1, Date_t::fromMillisSinceEpoch(555), Date_t::fromMillisSinceEpoch(554));
        // Negative date.
        assertComparison(1, Date_t::fromMillisSinceEpoch(0), Date_t::fromMillisSinceEpoch(-1));

        // Regex.
        assertComparison(0, fromjson("{'':/a/}"), fromjson("{'':/a/}"));
        assertComparison(-1, fromjson("{'':/a/}"), fromjson("{'':/a/i}"));
        assertComparison(-1, fromjson("{'':/a/}"), fromjson("{'':/aa/}"));

        // Timestamp.
        assertComparison(0, Timestamp(1234), Timestamp(1234));
        assertComparison(-1, Timestamp(4), Timestamp(1234));
        // High bit set.
        assertComparison(1, Timestamp(~0U, 2), Timestamp(0, 3));

        // Cross-type comparisons. Listed in order of canonical types.
        assertComparison(-1, Value(mongo::MINKEY), Value());
        assertComparison(0, Value(), Value());
        assertComparison(0, Value(), Value(BSONUndefined));
        assertComparison(-1, Value(BSONUndefined), Value(BSONNULL));
        assertComparison(-1, Value(BSONNULL), Value(1));
        assertComparison(0, Value(1), Value(1LL));
        assertComparison(0, Value(1), Value(1.0));
        assertComparison(-1, Value(1), Value("string"_sd));
        assertComparison(0, Value("string"_sd), Value(BSONSymbol("string")));
        assertComparison(-1, Value("string"_sd), Value(mongo::Document()));
        assertComparison(-1, Value(mongo::Document()), Value(vector<Value>()));
        assertComparison(-1, Value(vector<Value>()), Value(BSONBinData("", 0, MD5Type)));
        assertComparison(-1, Value(BSONBinData("", 0, MD5Type)), Value(mongo::OID()));
        assertComparison(-1, Value(mongo::OID()), Value(false));
        assertComparison(-1, Value(false), Value(Date_t()));
        assertComparison(-1, Value(Date_t()), Value(Timestamp()));
        assertComparison(-1, Value(Timestamp()), Value(BSONRegEx("")));
        assertComparison(-1, Value(BSONRegEx("")), Value(BSONDBRef("", mongo::OID())));
        assertComparison(-1, Value(BSONDBRef("", mongo::OID())), Value(BSONCode("")));
        assertComparison(-1, Value(BSONCode("")), Value(BSONCodeWScope("", BSONObj())));
        assertComparison(-1, Value(BSONCodeWScope("", BSONObj())), Value(mongo::MAXKEY));
    }

private:
    template <class T, class U>
    void assertComparison(int expectedResult, const T& a, const U& b) {
        assertComparison(expectedResult, BSON("" << a), BSON("" << b));
    }
    void assertComparison(int expectedResult, const Timestamp& a, const Timestamp& b) {
        BSONObjBuilder first;
        first.append("", a);
        BSONObjBuilder second;
        second.append("", b);
        assertComparison(expectedResult, first.obj(), second.obj());
    }
    int sign(int cmp) {
        if (cmp == 0)
            return 0;
        else if (cmp < 0)
            return -1;
        else
            return 1;
    }
    int cmp(const Value& a, const Value& b) {
        return sign(ValueComparator().compare(a, b));
    }
    void assertComparison(int expectedResult, const BSONObj& a, const BSONObj& b) {
        assertComparison(expectedResult, fromBson(a), fromBson(b));
    }
    void assertComparison(int expectedResult, const Value& a, const Value& b) {
        LOGV2(20586, "testing {a} and {b}", "a"_attr = a.toString(), "b"_attr = b.toString());

        // reflexivity
        ASSERT_EQUALS(0, cmp(a, a));
        ASSERT_EQUALS(0, cmp(b, b));

        // symmetry
        ASSERT_EQUALS(expectedResult, cmp(a, b));
        ASSERT_EQUALS(-expectedResult, cmp(b, a));

        if (expectedResult == 0) {
            // equal values must hash equally.
            ASSERT_EQUALS(hash(a), hash(b));
        } else {
            // unequal values must hash unequally.
            // (not true in general but we should error if it fails in any of these cases)
            ASSERT_NOT_EQUALS(hash(a), hash(b));
        }

        // same as BSON
        ASSERT_EQUALS(expectedResult,
                      sign(toBson(a).firstElement().woCompare(toBson(b).firstElement())));
    }
    size_t hash(const Value& v) {
        size_t seed = 0xf00ba6;
        const StringData::ComparatorInterface* stringComparator = nullptr;
        v.hash_combine(seed, stringComparator);
        return seed;
    }
};

class SubFields {
public:
    void run() {
        const Value val = fromBson(fromjson("{'': {a: [{x:1, b:[1, {y:1, c:1234, z:1}, 1]}]}}"));
        // ^ this outer object is removed by fromBson

        ASSERT(val.getType() == mongo::Object);

        ASSERT(val[999].missing());
        ASSERT(val["missing"].missing());
        ASSERT(val["a"].getType() == mongo::Array);

        ASSERT(val["a"][999].missing());
        ASSERT(val["a"]["missing"].missing());
        ASSERT(val["a"][0].getType() == mongo::Object);

        ASSERT(val["a"][0][999].missing());
        ASSERT(val["a"][0]["missing"].missing());
        ASSERT(val["a"][0]["b"].getType() == mongo::Array);

        ASSERT(val["a"][0]["b"][999].missing());
        ASSERT(val["a"][0]["b"]["missing"].missing());
        ASSERT(val["a"][0]["b"][1].getType() == mongo::Object);

        ASSERT(val["a"][0]["b"][1][999].missing());
        ASSERT(val["a"][0]["b"][1]["missing"].missing());
        ASSERT(val["a"][0]["b"][1]["c"].getType() == mongo::NumberInt);
        ASSERT_EQUALS(val["a"][0]["b"][1]["c"].getInt(), 1234);
    }
};


class SerializationOfMissingForSorter {
    // Can't be tested in AllTypesDoc since missing values are omitted when adding to BSON.
public:
    void run() {
        const Value missing;
        const Value arrayOfMissing = Value(vector<Value>(10));

        BufBuilder bb;
        missing.serializeForSorter(bb);
        arrayOfMissing.serializeForSorter(bb);

        BufReader reader(bb.buf(), bb.len());
        ASSERT_VALUE_EQ(missing,
                        Value::deserializeForSorter(reader, Value::SorterDeserializeSettings()));
        ASSERT_VALUE_EQ(arrayOfMissing,
                        Value::deserializeForSorter(reader, Value::SorterDeserializeSettings()));
    }
};

namespace {

// Integer limits.
const int kIntMax = std::numeric_limits<int>::max();
const int kIntMin = std::numeric_limits<int>::lowest();
const long long kIntMaxAsLongLong = kIntMax;
const long long kIntMinAsLongLong = kIntMin;
const double kIntMaxAsDouble = kIntMax;
const double kIntMinAsDouble = kIntMin;
const Decimal128 kIntMaxAsDecimal = Decimal128(kIntMax);
const Decimal128 kIntMinAsDecimal = Decimal128(kIntMin);

// 64-bit integer limits.
const long long kLongLongMax = std::numeric_limits<long long>::max();
const long long kLongLongMin = std::numeric_limits<long long>::lowest();
const double kLongLongMaxAsDouble = static_cast<double>(kLongLongMax);
const double kLongLongMinAsDouble = static_cast<double>(kLongLongMin);
const Decimal128 kLongLongMaxAsDecimal = Decimal128(static_cast<int64_t>(kLongLongMax));
const Decimal128 kLongLongMinAsDecimal = Decimal128(static_cast<int64_t>(kLongLongMin));

// Double limits.
const double kDoubleMax = std::numeric_limits<double>::max();
const double kDoubleMin = std::numeric_limits<double>::lowest();
const Decimal128 kDoubleMaxAsDecimal = Decimal128(kDoubleMin);
const Decimal128 kDoubleMinAsDecimal = Decimal128(kDoubleMin);

}  // namespace

TEST(ValueIntegral, CorrectlyIdentifiesValidIntegralValues) {
    ASSERT_TRUE(Value(kIntMax).integral());
    ASSERT_TRUE(Value(kIntMin).integral());
    ASSERT_TRUE(Value(kIntMaxAsLongLong).integral());
    ASSERT_TRUE(Value(kIntMinAsLongLong).integral());
    ASSERT_TRUE(Value(kIntMaxAsDouble).integral());
    ASSERT_TRUE(Value(kIntMinAsDouble).integral());
    ASSERT_TRUE(Value(kIntMaxAsDecimal).integral());
    ASSERT_TRUE(Value(kIntMinAsDecimal).integral());
}

TEST(ValueIntegral, CorrectlyIdentifiesInvalidIntegralValues) {
    ASSERT_FALSE(Value(kLongLongMax).integral());
    ASSERT_FALSE(Value(kLongLongMin).integral());
    ASSERT_FALSE(Value(kLongLongMaxAsDouble).integral());
    ASSERT_FALSE(Value(kLongLongMinAsDouble).integral());
    ASSERT_FALSE(Value(kLongLongMaxAsDecimal).integral());
    ASSERT_FALSE(Value(kLongLongMinAsDecimal).integral());
    ASSERT_FALSE(Value(kDoubleMax).integral());
    ASSERT_FALSE(Value(kDoubleMin).integral());
}

TEST(ValueIntegral, CorrectlyIdentifiesValid64BitIntegralValues) {
    ASSERT_TRUE(Value(kIntMax).integral64Bit());
    ASSERT_TRUE(Value(kIntMin).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMax).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMin).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMinAsDouble).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMaxAsDecimal).integral64Bit());
    ASSERT_TRUE(Value(kLongLongMinAsDecimal).integral64Bit());
}

TEST(ValueIntegral, CorrectlyIdentifiesInvalid64BitIntegralValues) {
    ASSERT_FALSE(Value(kLongLongMaxAsDouble).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMax).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMin).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMaxAsDecimal).integral64Bit());
    ASSERT_FALSE(Value(kDoubleMinAsDecimal).integral64Bit());
}

}  // namespace Value

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("document") {}

    void setupTests() {
        add<Document::AddField>();
        add<Document::GetValue>();
        add<Document::SetField>();
        add<Document::Compare>();
        add<Document::Clone>();
        add<Document::CloneMultipleFields>();
        add<Document::FieldIteratorEmpty>();
        add<Document::FieldIteratorSingle>();
        add<Document::FieldIteratorMultiple>();
        add<Document::AllTypesDoc>();

        add<Value::BSONArrayTest>();
        add<Value::Int>();
        add<Value::Long>();
        add<Value::Double>();
        add<Value::String>();
        add<Value::StringWithNull>();
        add<Value::LongString>();
        add<Value::Date>();
        add<Value::JSTimestamp>();
        add<Value::EmptyDocument>();
        add<Value::EmptyArray>();
        add<Value::Array>();
        add<Value::Oid>();
        add<Value::Bool>();
        add<Value::Regex>();
        add<Value::Symbol>();
        add<Value::Undefined>();
        add<Value::Null>();
        add<Value::True>();
        add<Value::False>();
        add<Value::MinusOne>();
        add<Value::Zero>();
        add<Value::One>();

        add<Value::Coerce::ZeroIntToBool>();
        add<Value::Coerce::NonZeroIntToBool>();
        add<Value::Coerce::ZeroLongToBool>();
        add<Value::Coerce::NonZeroLongToBool>();
        add<Value::Coerce::ZeroDoubleToBool>();
        add<Value::Coerce::NonZeroDoubleToBool>();
        add<Value::Coerce::StringToBool>();
        add<Value::Coerce::ObjectToBool>();
        add<Value::Coerce::ArrayToBool>();
        add<Value::Coerce::DateToBool>();
        add<Value::Coerce::RegexToBool>();
        add<Value::Coerce::TrueToBool>();
        add<Value::Coerce::FalseToBool>();
        add<Value::Coerce::NullToBool>();
        add<Value::Coerce::UndefinedToBool>();
        add<Value::Coerce::IntToInt>();
        add<Value::Coerce::LongToInt>();
        add<Value::Coerce::DoubleToInt>();
        add<Value::Coerce::NullToInt>();
        add<Value::Coerce::UndefinedToInt>();
        add<Value::Coerce::StringToInt>();
        add<Value::Coerce::MaxIntToInt>();
        add<Value::Coerce::MinIntToInt>();
        add<Value::Coerce::TooLargeToInt>();
        add<Value::Coerce::TooLargeNegativeToInt>();
        add<Value::Coerce::IntToLong>();
        add<Value::Coerce::LongToLong>();
        add<Value::Coerce::DoubleToLong>();
        add<Value::Coerce::NullToLong>();
        add<Value::Coerce::UndefinedToLong>();
        add<Value::Coerce::StringToLong>();
        add<Value::Coerce::InfToLong>();
        add<Value::Coerce::NegInfToLong>();
        add<Value::Coerce::InvalidLargeToLong>();
        add<Value::Coerce::LowestDoubleToLong>();
        add<Value::Coerce::TowardsInfinityToLong>();
        add<Value::Coerce::IntToDouble>();
        add<Value::Coerce::LongToDouble>();
        add<Value::Coerce::DoubleToDouble>();
        add<Value::Coerce::NullToDouble>();
        add<Value::Coerce::UndefinedToDouble>();
        add<Value::Coerce::StringToDouble>();
        add<Value::Coerce::DateToDate>();
        add<Value::Coerce::TimestampToDate>();
        add<Value::Coerce::StringToDate>();
        add<Value::Coerce::DoubleToString>();
        add<Value::Coerce::IntToString>();
        add<Value::Coerce::LongToString>();
        add<Value::Coerce::StringToString>();
        add<Value::Coerce::TimestampToString>();
        add<Value::Coerce::DateToString>();
        add<Value::Coerce::NullToString>();
        add<Value::Coerce::UndefinedToString>();
        add<Value::Coerce::DocumentToString>();
        add<Value::Coerce::TimestampToTimestamp>();
        add<Value::Coerce::DateToTimestamp>();

        add<Value::GetWidestNumeric>();
        add<Value::AddToBsonObj>();
        add<Value::AddToBsonArray>();
        add<Value::Compare>();
        add<Value::SubFields>();
        add<Value::SerializationOfMissingForSorter>();
    }
};

OldStyleSuiteInitializer<All> myall;

}  // namespace DocumentTests
