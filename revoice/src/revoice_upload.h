#pragma once

// External-server dump subsystem. Activated only when the user types
// `rv_upload_dump` in the server console — the cmd is registered in
// Revoice_Load. There is no per-frame work, no hook, no auto-init:
// every code path in revoice_upload.cpp is reachable only via this one
// function call.
//
// Logs go to cstrike/addons/amxmodx/logs/RV_upload_YYYYMMDD.log via
// RvLogUpload(), written directly from the worker thread.
void Cmd_UploadDump();

// Auto-dump scheduler. Call at every map boundary (ServerActivate is a good
// hook). No-op unless REV_AutoUploadDump is 1. When active, fires
// Cmd_UploadDump at most once per local calendar day, gated on hour-of-day
// >= 4 to keep heavy work pinned to off-peak. State is persisted in
// cstrike/addons/amxmodx/logs/rv_last_dump.txt so the per-day check
// survives a server restart.
void Revoice_AutoDump_MaybeTrigger();
