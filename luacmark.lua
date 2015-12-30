-- local inspect = require('inspect').inspect -- TODO debugging

local cmark = require("cmark")
local yaml = require("yaml")
local Lust = require("lust")

local luacmark = {}

luacmark.version = "0.23"

luacmark.writers = {
  html = function(d, opts, _cols) return cmark.render_html(d, opts) end,
  man = cmark.render_man,
  xml = function(d, opts, _cols) return cmark.render_xml(d, opts) end,
  latex = cmark.render_latex,
  commonmark = cmark.render_commonmark
}

luacmark.defaults = {
  smart = false,
  hardbreaks = false,
  sourcepos = false,
  safe = false,
  columns = 0,
}

local toOptions = function(opts)
  if type(opts) == 'table' then
    return (cmark.OPT_VALIDATE_UTF8 + cmark.OPT_NORMALIZE +
      (opts.smart and cmark.OPT_SMART or 0) +
      (opts.safe and cmark.OPT_SAFE or 0) +
      (opts.hardbreaks and cmark.OPT_HARDBREAKS or 0) +
      (opts.sourcepos and cmark.OPT_SOURCEPOS or 0)
      )
  else
     return opts
  end
end

-- walk nodes of table, applying a callback to each
local function walk_table(table, callback, inplace)
  assert(type(table) == 'table')
  local new = {}
  local res
  for k, v in pairs(table) do
    if type(v) == 'table' then
      res = walk_table(v, callback, inplace)
    else
      res = callback(v)
    end
    if not inplace then
      new[k] = res
    end
  end
  if not inplace then
    return new
  end
end


local defaultEnv = _ENV
table.insert(defaultEnv, cmark)

-- Run the specified filter, with source 'source' (a string
-- or function returning chunks) and name 'name', on the cmark
-- node 'doc', with output format 'to'.  The filter modifies
-- 'doc' destructively.  Returns true if successful, otherwise
-- false and an error message.
function luacmark.run_filter(source, name, doc, to)
  local result, msg = load(source, name, 't', defaultEnv)
  if result then
    result()(doc, to)
    return true
  else
    return false, msg
  end
end

--- Find a template and return its contents (or `false` if
-- not found). The template is sought first in the
-- working directory, then in `templates`, then in
-- `$HOME/.lunamark/templates`, then in the Windows
-- `APPDATA` directory.
function luacmark.find_template(name, to)
  if not name then
    return false, "Missing template name"
  end
  local base, ext = name:match("([^%.]*)(.*)")
  if (not ext or ext == "") and to then
    ext = "." .. to
  end
  local fname = base .. ext
  local file = io.open(fname, "r")
  if not file then
    file = io.open("templates/" .. fname, "r")
  end
  if not file then
    local home = os.getenv("HOME")
    if home then
      file = io.open(home .. "/.luacmark/templates/" .. fname, "r")
    end
  end
  if not file then
    local appdata = os.getenv("APPDATA")
    if appdata then
      file = io.open(appdata .. "/luacmark/templates/" .. fname, "r")
    end
  end
  if file then
    return file:read("*all")
  else
    return false, "Could not find template '" .. fname .. "'"
  end
end

-- Render a metadata node in the target format.
local render_metadata = function(node, writer, options, columns)
  local firstblock = cmark.node_first_child(node)
  if cmark.node_get_type(firstblock) == cmark.NODE_PARAGRAPH and
     not cmark.node_next(firstblock) then
     -- render as inlines
     local ils = cmark.node_new(cmark.NODE_CUSTOM_INLINE)
     local b = cmark.node_first_child(firstblock)
     while b do
        local nextb = cmark.node_next(b)
        cmark.node_append_child(ils, b)
        b = nextb
     end
     local result = string.gsub(writer(ils, options, columns), "%s*$", "")
     cmark.node_free(ils)
     return result
  else -- render as blocks
     return writer(node, options, columns)
  end
end

-- Iterate over the metadata, converting to cmark nodes.
-- Returns a new table.
local convert_metadata = function(table, options)
  return walk_table(table,
                    function(s)
                      return cmark.parse_string(tostring(s), options)
                    end, false)
end

-- Parses document with optional front YAML metadata; returns document,
-- metadata.
local parse_document_with_metadata = function(inp, options)
  local metadata = {}
  if string.find(inp, '^---[\r\n]') then
    local _, endlast = string.find(inp, '[\r\n]...[ \t]*[\r\n][\r\n\t ]*', 3)
    if not endlast then
      local _, endlast = string.find(inp, '[\r\n]---[ \t]*[\r\n][\r\n\t ]*', 3)
    end
    if endlast then
      local yaml_meta = yaml.load(string.sub(inp, 1, endlast))
      metadata = convert_metadata(yaml_meta, options)
      if type(metadata) ~= 'table' then
        metadata = {}
      end
      if yaml_meta then
        -- We insert blank lines where the header was, so sourcepos is accurate:
        inp = string.gsub(string.sub(inp, 1, endlast - 1), '[^\n\r]+', '') ..
           string.sub(inp, endlast)
      end
    end
  end
  doc = cmark.parse_document(inp, string.len(inp), options)
  return doc, metadata
end

-- 'inp' is the string input source.
-- 'options' is a table with fields 'smart', 'hardbreaks',
-- 'safe', 'sourcepos' (all boolean) and 'columns' (number,
-- 0 for no wrapping).
-- 'callback' is a filter or nil.
-- 'template' is a Lust template to be filled with the document
-- body and rendered metadata, or nil.
-- TODO handle errors
function luacmark.convert(inp, to, options, callback, template)
  local opts = toOptions(options)
  local columns = options.columns or 0
  local doc, meta = parse_document_with_metadata(inp, opts)
  if callback then
    -- apply callback to nodes of metadata
    walk_table(meta, function(node) callback(node, to) end, true)
    callback(doc, to)
  end
  local writer = luacmark.writers[to]
  local body = writer(doc, opts, columns)
  local data = walk_table(meta,
                          function(node)
                            local res = render_metadata(node, writer, opts, columns)
                            return res
                          end, false)
  -- free memory allocated by libcmark
  cmark.node_free(doc)
  walk_table(meta, cmark.node_free, true)
  if template then
    data.body = body
    return Lust(template):gen(data)
  else
    return body
  end
end

return luacmark
