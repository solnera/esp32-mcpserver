#include <unity.h>

#include <ArduinoJson.h>
#include <cstring>

#include "MCPServer.h"

/* Covers the fluent Schema builder that produces a tool's inputSchema /
 * outputSchema. Every assertion is made against the serialized JSON, because
 * that is exactly what tools/list puts on the wire. */

namespace {

void serializeSchema(Schema schema, JsonDocument& out) {
    JsonDocument built = schema.build();
    std::string json;
    serializeJson(built, json);
    TEST_ASSERT_FALSE(deserializeJson(out, json));
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

/* ======== Scalar types ======== */

void test_scalar_types_emit_their_type(void) {
    struct {
        Schema (*factory)();
        const char* expected;
    } cases[] = {
        {&Schema::string, "string"}, {&Schema::integer, "integer"}, {&Schema::number, "number"},
        {&Schema::boolean, "boolean"}, {&Schema::array, "array"},  {&Schema::null, "null"},
    };

    for (const auto& c : cases) {
        JsonDocument doc;
        serializeSchema(c.factory(), doc);
        TEST_ASSERT_EQUAL_STRING(c.expected, doc["type"].as<const char*>());
    }
}

void test_object_starts_with_an_empty_properties_map(void) {
    JsonDocument doc;
    serializeSchema(Schema::object(), doc);

    TEST_ASSERT_EQUAL_STRING("object", doc["type"].as<const char*>());
    TEST_ASSERT_TRUE(doc["properties"].is<JsonObjectConst>());
    TEST_ASSERT_EQUAL_UINT(0, doc["properties"].as<JsonObjectConst>().size());
}

/* ======== Common modifiers ======== */

void test_title_description_format(void) {
    JsonDocument doc;
    serializeSchema(Schema::string().title("Email").description("User email").format("email"), doc);

    TEST_ASSERT_EQUAL_STRING("string", doc["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Email", doc["title"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("User email", doc["description"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("email", doc["format"].as<const char*>());
}

void test_default_value_keeps_its_json_type(void) {
    JsonDocument text;
    serializeSchema(Schema::string().defaultValue("red"), text);
    TEST_ASSERT_EQUAL_STRING("red", text["default"].as<const char*>());

    JsonDocument num;
    serializeSchema(Schema::integer().defaultValue(7), num);
    TEST_ASSERT_TRUE(num["default"].is<int>());
    TEST_ASSERT_EQUAL_INT(7, num["default"].as<int>());

    /* A false default must survive as false rather than being dropped as an
     * empty value — the Properties builder this replaced stored defaults as
     * strings and could not express that. */
    JsonDocument flag;
    serializeSchema(Schema::boolean().defaultValue(false), flag);
    TEST_ASSERT_TRUE(flag["default"].is<bool>());
    TEST_ASSERT_FALSE(flag["default"].as<bool>());
}

/* ======== Objects ======== */

void test_object_with_properties_and_required(void) {
    JsonDocument doc;
    serializeSchema(Schema::object()
                        .description("Echo parameters")
                        .property("text", Schema::string().description("Text to echo"))
                        .property("count", Schema::integer())
                        .required({"text"}),
                    doc);

    TEST_ASSERT_EQUAL_STRING("Echo parameters", doc["description"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string", doc["properties"]["text"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Text to echo", doc["properties"]["text"]["description"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("integer", doc["properties"]["count"]["type"].as<const char*>());

    JsonArrayConst required = doc["required"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(1, required.size());
    TEST_ASSERT_EQUAL_STRING("text", required[0].as<const char*>());
}

void test_required_accumulates_across_calls(void) {
    JsonDocument doc;
    serializeSchema(Schema::object()
                        .property("a", Schema::string())
                        .property("b", Schema::string())
                        .required({"a"})
                        .required({"b"}),
                    doc);

    JsonArrayConst required = doc["required"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(2, required.size());
    TEST_ASSERT_EQUAL_STRING("a", required[0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("b", required[1].as<const char*>());
}

void test_additional_properties_emits_both_polarities(void) {
    JsonDocument closed;
    serializeSchema(Schema::object().additionalProperties(false), closed);
    TEST_ASSERT_TRUE(closed["additionalProperties"].is<bool>());
    TEST_ASSERT_FALSE(closed["additionalProperties"].as<bool>());

    JsonDocument open;
    serializeSchema(Schema::object().additionalProperties(true), open);
    TEST_ASSERT_TRUE(open["additionalProperties"].as<bool>());

    /* Unset means absent, not `true` — an unconstrained schema must not claim
     * anything the caller did not ask for. */
    JsonDocument bare;
    serializeSchema(Schema::object(), bare);
    TEST_ASSERT_TRUE(bare["additionalProperties"].isNull());
}

void test_deeply_nested_objects(void) {
    JsonDocument doc;
    serializeSchema(Schema::object().property(
                        "level1", Schema::object().property(
                                      "level2", Schema::object().property("leaf", Schema::string().description("deep")))),
                    doc);

    JsonVariantConst leaf = doc["properties"]["level1"]["properties"]["level2"]["properties"]["leaf"];
    TEST_ASSERT_EQUAL_STRING("string", leaf["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("deep", leaf["description"].as<const char*>());
}

/* ======== Arrays ======== */

void test_array_items_and_bounds(void) {
    JsonDocument doc;
    serializeSchema(Schema::array().items(Schema::string()).minItems(1).maxItems(10), doc);

    TEST_ASSERT_EQUAL_STRING("array", doc["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("string", doc["items"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(1, doc["minItems"].as<int>());
    TEST_ASSERT_EQUAL_INT(10, doc["maxItems"].as<int>());
}

void test_array_of_objects(void) {
    JsonDocument doc;
    serializeSchema(Schema::array().items(Schema::object().property("id", Schema::integer()).required({"id"})),
                    doc);

    TEST_ASSERT_EQUAL_STRING("object", doc["items"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("integer", doc["items"]["properties"]["id"]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("id", doc["items"]["required"][0].as<const char*>());
}

/* ======== Constraints ======== */

void test_numeric_constraints(void) {
    JsonDocument doc;
    serializeSchema(Schema::number().minimum(0).maximum(100).exclusiveMinimum(-1).exclusiveMaximum(101).multipleOf(0.5),
                    doc);

    // Unity's double asserts are compiled out by default; float is exact for
    // every value under test here.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, doc["minimum"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(100.0f, doc["maximum"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, doc["exclusiveMinimum"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(101.0f, doc["exclusiveMaximum"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(0.5f, doc["multipleOf"].as<float>());
}

void test_string_constraints(void) {
    JsonDocument doc;
    serializeSchema(Schema::string().minLength(2).maxLength(64).pattern("^[a-z]+$"), doc);

    TEST_ASSERT_EQUAL_INT(2, doc["minLength"].as<int>());
    TEST_ASSERT_EQUAL_INT(64, doc["maxLength"].as<int>());
    TEST_ASSERT_EQUAL_STRING("^[a-z]+$", doc["pattern"].as<const char*>());
}

void test_enum_of_strings_and_numbers(void) {
    JsonDocument text;
    serializeSchema(Schema::string().enumValues({"red", "green", "blue"}), text);
    JsonArrayConst values = text["enum"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(3, values.size());
    TEST_ASSERT_EQUAL_STRING("red", values[0].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("blue", values[2].as<const char*>());

    JsonDocument nums;
    serializeSchema(Schema::integer().enumValues({1, 2, 3}), nums);
    JsonArrayConst numValues = nums["enum"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(3, numValues.size());
    TEST_ASSERT_TRUE(numValues[0].is<int>());
    TEST_ASSERT_EQUAL_INT(2, numValues[1].as<int>());
}

/* ======== Composition ======== */

void test_one_of_any_of_all_of(void) {
    JsonDocument oneOf;
    serializeSchema(Schema().oneOf({Schema::string(), Schema::integer()}), oneOf);
    TEST_ASSERT_EQUAL_UINT(2, oneOf["oneOf"].as<JsonArrayConst>().size());
    TEST_ASSERT_EQUAL_STRING("string", oneOf["oneOf"][0]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("integer", oneOf["oneOf"][1]["type"].as<const char*>());

    JsonDocument anyOf;
    serializeSchema(Schema().anyOf({Schema::boolean(), Schema::null()}), anyOf);
    TEST_ASSERT_EQUAL_STRING("boolean", anyOf["anyOf"][0]["type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("null", anyOf["anyOf"][1]["type"].as<const char*>());

    JsonDocument allOf;
    serializeSchema(Schema().allOf({Schema::object().property("a", Schema::string())}), allOf);
    TEST_ASSERT_EQUAL_STRING("string", allOf["allOf"][0]["properties"]["a"]["type"].as<const char*>());
}

void test_default_constructed_schema_is_unconstrained(void) {
    /* An omitted type is a valid, unconstrained JSON Schema; it must not be
     * forced to emit an empty string type. */
    JsonDocument doc;
    serializeSchema(Schema(), doc);
    TEST_ASSERT_TRUE(doc["type"].isNull());
}

/* ======== Independence ======== */

void test_property_schemas_are_copied_not_aliased(void) {
    /* property() takes its argument by value and stores a copy, so mutating the
     * source afterwards must not reach back into the parent. */
    Schema child = Schema::string().description("original");
    Schema parent = Schema::object().property("field", child);
    child.description("mutated");

    JsonDocument doc;
    serializeSchema(std::move(parent), doc);
    TEST_ASSERT_EQUAL_STRING("original", doc["properties"]["field"]["description"].as<const char*>());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_scalar_types_emit_their_type);
    RUN_TEST(test_object_starts_with_an_empty_properties_map);
    RUN_TEST(test_title_description_format);
    RUN_TEST(test_default_value_keeps_its_json_type);
    RUN_TEST(test_object_with_properties_and_required);
    RUN_TEST(test_required_accumulates_across_calls);
    RUN_TEST(test_additional_properties_emits_both_polarities);
    RUN_TEST(test_deeply_nested_objects);
    RUN_TEST(test_array_items_and_bounds);
    RUN_TEST(test_array_of_objects);
    RUN_TEST(test_numeric_constraints);
    RUN_TEST(test_string_constraints);
    RUN_TEST(test_enum_of_strings_and_numbers);
    RUN_TEST(test_one_of_any_of_all_of);
    RUN_TEST(test_default_constructed_schema_is_unconstrained);
    RUN_TEST(test_property_schemas_are_copied_not_aliased);
    return UNITY_END();
}
