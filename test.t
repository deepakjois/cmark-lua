#!/usr/bin/env lua
require 'Test.More'

package.path = "./?.lua;" .. package.path
package.cpath = "./?.so;" .. package.cpath

local cmark = require 'cmark'
local builder = require 'cmark.builder'
local tests = require 'spec-tests'

for _,test in ipairs(tests) do
  local doc  = cmark.parse_string(test.markdown, cmark.OPT_DEFAULT)
  local html = cmark.render_html(doc, cmark.OPT_DEFAULT)
  is(html, test.html, "example " .. tostring(test.example) ..
           " (lines " .. tostring(test.start_line) .. " - " ..
           tostring(test.end_line) .. ")")
end

local tohtml = function(node)
  return cmark.render_html(node, cmark.OPT_DEFAULT)
end

is(tohtml(builder.text("hello")), "hello", "builder.text")
 is(tohtml(builder.link({url = "url", link_text = "hello"})),
    '<a href="url">hello</a>', "builder.link")


done_testing()
