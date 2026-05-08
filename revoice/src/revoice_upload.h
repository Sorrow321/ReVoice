#pragma once

bool Revoice_Upload_Init();
void Cmd_UploadDump();

// Must be called once per server frame from the main thread. Drains messages
// queued by the upload worker thread and writes them to logs/L*.log via
// UTIL_LogPrintf (which is not safe to call directly from the worker).
void Revoice_Upload_DrainLog();
