-- baker-liveops on Nakama: published overlays as storage objects, one collection per
-- environment ("liveops.prod"), owned by the system user, unreadable by clients directly.
--
--   liveops_manifest  {"env": "prod"}                          -> {"body": manifest}
--   liveops_overlay   {"env": "prod", "key": "overlays/x.dta"} -> {"body": overlay}
--   liveops_put       {"env", "key", "body"}  server to server only (the runtime http_key)
--
-- The bodies are stored as published: the client verifies the manifest's signature and each
-- overlay's hash, so the server is trusted for availability, not for content.

local nk = require("nakama")

local SYSTEM = "00000000-0000-0000-0000-000000000000"
local NOT_FOUND, INVALID, DENIED = 5, 3, 7

local function args(payload)
  local ok, p = pcall(nk.json_decode, payload or "")
  if not ok or type(p) ~= "table" then error({ "payload is not a JSON object", INVALID }) end
  if type(p.env) ~= "string" or not p.env:match("^[%w_.%-]+$") or #p.env > 64 then
    error({ "env is a name", INVALID })
  end
  return p
end

local function valid_key(key)
  return type(key) == "string" and #key <= 120 and
      (key == "manifest" or key:match("^overlays/[%w_.%-]+%.dta$") ~= nil)
end

local function read(env, key)
  local objects = nk.storage_read({ { collection = "liveops." .. env, key = key, user_id = SYSTEM } })
  if #objects == 0 then error({ "no " .. key .. " in " .. env, NOT_FOUND }) end
  return nk.json_encode({ body = objects[1].value.body })
end

local function manifest(_, payload)
  return read(args(payload).env, "manifest")
end

local function overlay(_, payload)
  local p = args(payload)
  if not valid_key(p.key) or p.key == "manifest" then error({ "key is overlays/NAME.dta", INVALID }) end
  return read(p.env, p.key)
end

local function put(context, payload)
  if context.user_id ~= nil and context.user_id ~= "" then
    error({ "publishing is server to server", DENIED })
  end
  local p = args(payload)
  if not valid_key(p.key) or type(p.body) ~= "string" then error({ "key and body", INVALID }) end
  nk.storage_write({ {
    collection = "liveops." .. p.env, key = p.key, user_id = SYSTEM,
    value = { body = p.body }, permission_read = 0, permission_write = 0,
  } })
  return nk.json_encode({ ok = true })
end

nk.register_rpc(manifest, "liveops_manifest")
nk.register_rpc(overlay, "liveops_overlay")
nk.register_rpc(put, "liveops_put")
