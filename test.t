#!/usr/bin/env lua
require 'Test.More'

package.path = "./?.lua;" .. package.path
package.cpath = "./?.so;" .. package.cpath

local cmark = require 'cmark'
local luacmark = require 'luacmark'
local tests = require 'spec-tests'

subtest("spec tests (cmark)", function()
  for _,test in ipairs(tests) do
    local doc  = cmark.parse_string(test.markdown, cmark.OPT_DEFAULT)
    local html = cmark.render_html(doc, cmark.OPT_DEFAULT)
    is(html, test.html, "example " .. tostring(test.example) ..
             " (lines " .. tostring(test.start_line) .. " - " ..
             tostring(test.end_line) .. ")")
  end
end)

subtest("spec tests (luacmark)", function()
  for _,test in ipairs(tests) do
    local html = luacmark.convert(test.markdown, 'html', {})
    is(html, test.html, "example " .. tostring(test.example) ..
             " (lines " .. tostring(test.start_line) .. " - " ..
             tostring(test.end_line) .. ")")
  end
end)

local body, meta, msg = luacmark.convert("Hello *world*", "latex", {})
is(body, "Hello \\emph{world}\n", "simple latex body")
eq_array(meta, {}, "simple latex meta")

local body, meta, msg = luacmark.convert("---\ntitle: My *title*\nauthor:\n- name: JJ\n  institute: U of H\n...\n\nHello *world*", "latex", {yaml_metadata = true})
is(body, "Hello \\emph{world}\n", "latex body")
eq_array(meta, {title = "My \\emph{title}", author = { {name = "JJ", institute = "U of H"}} }, "latex meta")

local body, meta, msg = luacmark.convert("---\ntitle: My *title*\nauthor:\n- name: JJ\n  institute: U of H\n...\n\nHello *world*", "latex", {yaml_metadata = false})
isnt(body, "Hello \\emph{world}\n", "latex body with yaml_metadata=false")
eq_array(meta, {}, "latex meta with yaml_metadata=false")

local body, meta, msg = luacmark.convert("---\ntitle: 1: 2\n...\n\nHello *world*", "latex", {yaml_metadata = true})
is(meta, nil, "latex body nil with bad yaml_metadata")
like(msg, "YAML parsing error:.*mapping values are not allowed in this context", "error message with bad yaml_metadata")

local badfilter, msg = luacmark.to_filter("function(x) return y end", "bad_filter.lua")
nok(badfilter, "to_filter fails on bad filter")
is(msg, "[string \"bad_filter.lua\"]:1: <name> expected near '('", "error message on bad filter")
local count_links = luacmark.to_filter(io.lines("filters/count_links.lua", 2^12), "count_links.lua")
ok(count_links, "loaded filter count_links.lua")
local body, meta, msg = luacmark.convert("[link](u) and <http://example.com>", "html", {filters = {count_links}})
is(body, "<p><a href=\"u\">link</a> (link #1) and <a href=\"http://example.com\">http://example.com</a> (link #2)</p>\n<p>2 links found in this html document.</p>\n", "added link numbers and count")

done_testing()