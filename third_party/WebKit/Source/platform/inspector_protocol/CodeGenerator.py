# Copyright 2016 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import os.path
import sys
import string
import optparse
import re
try:
    import json
except ImportError:
    import simplejson as json

# Path handling for libraries and templates
# Paths have to be normalized because Jinja uses the exact template path to
# determine the hash used in the cache filename, and we need a pre-caching step
# to be concurrency-safe. Use absolute path because __file__ is absolute if
# module is imported, and relative if executed directly.
# If paths differ between pre-caching and individual file compilation, the cache
# is regenerated, which causes a race condition and breaks concurrent build,
# since some compile processes will try to read the partially written cache.
module_path, module_filename = os.path.split(os.path.realpath(__file__))
templates_dir = module_path
third_party_dir = os.path.normpath(os.path.join(
    module_path, os.pardir, os.pardir, os.pardir, os.pardir))

# jinja2 is in chromium's third_party directory.
# Insert at 1 so at front to override system libraries, and
# after path[0] == invoking script dir
sys.path.insert(1, third_party_dir)
import jinja2

cmdline_parser = optparse.OptionParser()
cmdline_parser.add_option("--output_dir")
cmdline_parser.add_option("--template_dir")

try:
    arg_options, arg_values = cmdline_parser.parse_args()
    if (len(arg_values) != 1):
        raise Exception("Exactly one plain argument expected (found %s)" % len(arg_values))
    input_json_filename = arg_values[0]
    output_dirname = arg_options.output_dir
    if not output_dirname:
        raise Exception("Output directory must be specified")
except Exception:
    # Work with python 2 and 3 http://docs.python.org/py3k/howto/pyporting.html
    exc = sys.exc_info()[1]
    sys.stderr.write("Failed to parse command-line arguments: %s\n\n" % exc)
    sys.stderr.write("Usage: <script> --output_dir <output_dir> protocol.json\n")
    exit(1)

input_file = open(input_json_filename, "r")
json_string = input_file.read()
json_api = json.loads(json_string)


def to_title_case(name):
    return name[:1].upper() + name[1:]


def dash_to_camelcase(word):
    return ''.join(to_title_case(x) or '-' for x in word.split('-'))


def initialize_jinja_env(cache_dir):
    jinja_env = jinja2.Environment(
        loader=jinja2.FileSystemLoader(templates_dir),
        # Bytecode cache is not concurrency-safe unless pre-cached:
        # if pre-cached this is read-only, but writing creates a race condition.
        bytecode_cache=jinja2.FileSystemBytecodeCache(cache_dir),
        keep_trailing_newline=True,  # newline-terminate generated files
        lstrip_blocks=True,  # so can indent control flow tags
        trim_blocks=True)
    jinja_env.filters.update({"to_title_case": to_title_case, "dash_to_camelcase": dash_to_camelcase})
    jinja_env.add_extension('jinja2.ext.loopcontrols')
    return jinja_env


def output_file(file_name):
    return open(file_name, "w")


def patch_full_qualified_refs():
    def patch_full_qualified_refs_in_domain(json, domain_name):
        if isinstance(json, list):
            for item in json:
                patch_full_qualified_refs_in_domain(item, domain_name)

        if not isinstance(json, dict):
            return
        for key in json:
            if key != "$ref":
                patch_full_qualified_refs_in_domain(json[key], domain_name)
                continue
            if json["$ref"].find(".") == -1:
                json["$ref"] = domain_name + "." + json["$ref"]

    for domain in json_api["domains"]:
        patch_full_qualified_refs_in_domain(domain, domain["domain"])


def create_user_type_definition(domain_name, type):
    return {
        "return_type": "PassOwnPtr<protocol::%s::%s>" % (domain_name, type["id"]),
        "pass_type": "PassOwnPtr<protocol::%s::%s>" % (domain_name, type["id"]),
        "to_raw_type": "%s.get()",
        "to_pass_type": "%s.release()",
        "type": "OwnPtr<protocol::%s::%s>" % (domain_name, type["id"]),
        "raw_type": "protocol::%s::%s" % (domain_name, type["id"]),
        "raw_pass_type": "protocol::%s::%s*" % (domain_name, type["id"]),
        "raw_return_type": "protocol::%s::%s*" % (domain_name, type["id"]),
    }


def create_object_type_definition():
    return {
        "return_type": "PassRefPtr<protocol::DictionaryValue>",
        "pass_type": "PassRefPtr<protocol::DictionaryValue>",
        "to_raw_type": "%s",
        "to_pass_type": "%s.release()",
        "type": "RefPtr<protocol::DictionaryValue>",
        "raw_type": "RefPtr<protocol::DictionaryValue>",
        "raw_pass_type": "PassRefPtr<protocol::DictionaryValue>",
        "raw_return_type": "RefPtr<protocol::DictionaryValue>",
    }


def create_any_type_definition():
    return {
        "return_type": "PassRefPtr<protocol::Value>",
        "pass_type": "PassRefPtr<protocol::Value>",
        "to_pass_type": "%s.release()",
        "to_raw_type": "%s",
        "type": "RefPtr<protocol::Value>",
        "raw_type": "RefPtr<protocol::Value>",
        "raw_pass_type": "PassRefPtr<protocol::Value>",
        "raw_return_type": "RefPtr<protocol::Value>",
    }


def create_primitive_type_definition(type):
    if type == "string":
        return {
            "return_type": "String",
            "pass_type": "const String&",
            "to_pass_type": "%s",
            "to_raw_type": "%s",
            "type": "String",
            "raw_type": "String",
            "raw_pass_type": "const String&",
            "raw_return_type": "String",
        }

    typedefs = {
        "number": "double",
        "integer": "int",
        "boolean": "bool"
    }
    jsontypes = {
        "number": "TypeNumber",
        "integer": "TypeNumber",
        "boolean": "TypeBoolean",
    }
    return {
        "return_type": typedefs[type],
        "pass_type": typedefs[type],
        "to_pass_type": "%s",
        "to_raw_type": "%s",
        "type": typedefs[type],
        "raw_type": typedefs[type],
        "raw_pass_type": typedefs[type],
        "raw_return_type": typedefs[type],
    }

type_definitions = {}
type_definitions["string"] = create_primitive_type_definition("string")
type_definitions["number"] = create_primitive_type_definition("number")
type_definitions["integer"] = create_primitive_type_definition("integer")
type_definitions["boolean"] = create_primitive_type_definition("boolean")
type_definitions["object"] = create_object_type_definition()
type_definitions["any"] = create_any_type_definition()


def wrap_array_definition(type):
    return {
        "return_type": "PassOwnPtr<protocol::Array<%s>>" % type["raw_type"],
        "pass_type": "PassOwnPtr<protocol::Array<%s>>" % type["raw_type"],
        "to_raw_type": "%s.get()",
        "to_pass_type": "%s.release()",
        "type": "OwnPtr<protocol::Array<%s>>" % type["raw_type"],
        "raw_type": "protocol::Array<%s>" % type["raw_type"],
        "raw_pass_type": "protocol::Array<%s>*" % type["raw_type"],
        "raw_return_type": "protocol::Array<%s>*" % type["raw_type"],
        "create_type": "adoptPtr(new protocol::Array<%s>())" % type["raw_type"],
        "out_type": "protocol::Array<%s>&" % type["raw_type"],
    }


def create_type_definitions():
    for domain in json_api["domains"]:
        if not ("types" in domain):
            continue
        for type in domain["types"]:
            if type["type"] == "object":
                type_definitions[domain["domain"] + "." + type["id"]] = create_user_type_definition(domain["domain"], type)
            elif type["type"] == "array":
                items_type = type["items"]["type"]
                type_definitions[domain["domain"] + "." + type["id"]] = wrap_array_definition(type_definitions[items_type])
            else:
                type_definitions[domain["domain"] + "." + type["id"]] = create_primitive_type_definition(type["type"])

patch_full_qualified_refs()
create_type_definitions()


def type_definition(name):
    return type_definitions[name]


def resolve_type(property):
    if "$ref" in property:
        return type_definitions[property["$ref"]]
    if property["type"] == "array":
        return wrap_array_definition(resolve_type(property["items"]))
    return type_definitions[property["type"]]


def join_arrays(dict, keys):
    result = []
    for key in keys:
        if key in dict:
            result += dict[key]
    return result


def generate(class_name):
    template_context = {
        "class_name": class_name,
        "api": json_api,
        "join_arrays": join_arrays,
        "resolve_type": resolve_type,
        "type_definition": type_definition
    }
    h_template = jinja_env.get_template("/%s_h.template" % class_name)
    cpp_template = jinja_env.get_template("/%s_cpp.template" % class_name)
    h_file = output_file(output_dirname + "/" + class_name + ".h")
    cpp_file = output_file(output_dirname + "/" + class_name + ".cpp")
    h_file.write(h_template.render(template_context))
    cpp_file.write(cpp_template.render(template_context))
    h_file.close()
    cpp_file.close()


jinja_env = initialize_jinja_env(output_dirname)
generate("Dispatcher")
generate("Frontend")
generate("TypeBuilder")
