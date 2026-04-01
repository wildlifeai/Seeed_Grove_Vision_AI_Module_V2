# Deployment ID Refactor Proposal
#### CGP / Claude — 1 April 2026

## Background

A 128-bit UUID is used per camera deployment. It is written into JPEG EXIF data by `image_task.c` and is set by the smartphone app via the I2C CLI interface.

### Current implementation

The UUID is stored as 8 × uint16_t values in `op_parameter[]` at indices
`OP_PARAMETER_DEPLOYMENT_ID_CHUNK_1` (20) through `OP_PARAMETER_DEPLOYMENT_ID_CHUNK_8` (27).
These are saved to and loaded from `CONFIG.TXT` as ordinary index-value pairs:

```
20 1234
21 5678
...
27 abcd
```

`fatfs_getDeploymentId()` in `fatfs_task.c` reassembles the UUID string from these 8 chunks.
There is no dedicated setter — the app sets the values directly using `setop 20 <val>` through
`setop 27 <val>` (eight separate commands).

### Problem

Sending 8 separate `setop` commands for one logical value is clumsy. The GPS location was
recently refactored to use a single string line in CONFIG.TXT (prefixed `G `), with matching
`setgps`/`getgps` CLI commands. The deployment UUID should follow the same pattern.

---

## Proposed approach

### 1. New CONFIG.TXT line format

Add a UUID line identified by the prefix `I ` (for deployment **I**D):

```
I xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
```

This follows the exact same convention as the GPS `G ` line.

### 2. New module-level variable in `fatfs_task.c`

Add a static char array to hold the UUID string, initialised to the zero UUID:

```c
static char deployment_id_string[UUIDLENGTH] = DEPLOYMENT_ID_ZERO_UUID;
```

### 3. `load_configuration()` — detect `I ` prefix

Inside the existing `while (f_gets(...))` loop, add a case parallel to the GPS one:

```c
// Special case if the first 2 characters are 'I ' for deployment ID
if ((line[0] == 'I') && (line[1] == ' ')) {
    strncpy(deployment_id_string, &line[2], UUIDLENGTH - 1);
    deployment_id_string[UUIDLENGTH - 1] = '\0';
    // Strip any trailing newline
    char *nl = strchr(deployment_id_string, '\n');
    if (nl) *nl = '\0';
}
```

### 4. `save_configuration()` — write `I ` line

After the GPS line is written, add:

```c
// Write the deployment ID
snprintf(line, sizeof(line), "I %s\n", deployment_id_string);
f_write(&dirManager->configFile, line, strlen(line), &bytesWritten);
```

### 5. Modify `fatfs_getDeploymentId()`

Change the function to return the string form if it has been set (i.e. is not the zero UUID),
otherwise fall back to reconstructing from the chunks as now. This gives backward compatibility
with CONFIG.TXT files that only contain the chunk form.

```c
void fatfs_getDeploymentId(char *deployment_id_buffer, size_t buffer_size) {
    if (buffer_size < UUIDLENGTH) {
        deployment_id_buffer[0] = '\0';
        return;
    }

    // Prefer the string form if it has been set
    if (strcmp(deployment_id_string, DEPLOYMENT_ID_ZERO_UUID) != 0) {
        snprintf(deployment_id_buffer, buffer_size, "%s", deployment_id_string);
        return;
    }

    // Fall back to reconstructing from chunks (backward compatibility)
    uint16_t chunks[8];
    bool all_zero = true;
    for (int i = 0; i < 8; i++) {
        chunks[i] = fatfs_getOperationalParameter(OP_PARAMETER_DEPLOYMENT_ID_CHUNK_1 + i);
        if (chunks[i] != 0) all_zero = false;
    }
    if (all_zero) {
        snprintf(deployment_id_buffer, buffer_size, "%s", DEPLOYMENT_ID_ZERO_UUID);
        return;
    }
    snprintf(deployment_id_buffer, buffer_size,
             "%04x%04x-%04x-%04x-%04x-%04x%04x%04x",
             chunks[0], chunks[1], chunks[2], chunks[3],
             chunks[4], chunks[5], chunks[6], chunks[7]);
}
```

### 6. Add `fatfs_setDeploymentId()` to `fatfs_task.c` and `.h`

```c
// In fatfs_task.h:
void fatfs_setDeploymentId(const char *uuid_string);

// In fatfs_task.c:
void fatfs_setDeploymentId(const char *uuid_string) {
    strncpy(deployment_id_string, uuid_string, UUIDLENGTH - 1);
    deployment_id_string[UUIDLENGTH - 1] = '\0';
}
```

### 7. New CLI commands in `CLI-commands.c`

Following the exact pattern of `setgps`/`getgps`:

#### Command definitions (alongside xSetGps / xGetGps)

```c
static const CLI_Command_Definition_t xSetDid = {
    "setdid",
    "setdid <uuid>:\r\n Set deployment ID UUID string (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)\r\n",
    prvSetdid,
    1
};

static const CLI_Command_Definition_t xGetDid = {
    "getdid",
    "getdid:\r\n Get current deployment ID UUID string\r\n",
    prvGetdid,
    0
};
```

#### Function prototypes (alongside prvSetgps / prvGetgps)

```c
static BaseType_t prvSetdid(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
static BaseType_t prvGetdid(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString);
```

#### Implementations

```c
static BaseType_t prvSetdid(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
    const char *param;
    BaseType_t paramLen;

    param = FreeRTOS_CLIGetParameter(pcCommandString, 1, &paramLen);
    if (!param || paramLen == 0) {
        snprintf(pcWriteBuffer, xWriteBufferLen, "Error: No UUID string provided.\r\n");
        return pdFALSE;
    }
    if (paramLen != UUIDLENGTH - 1) {
        snprintf(pcWriteBuffer, xWriteBufferLen,
                 "Error: UUID must be %d characters (got %d).\r\n",
                 UUIDLENGTH - 1, (int)paramLen);
        return pdFALSE;
    }

    char uuid[UUIDLENGTH];
    strncpy(uuid, param, UUIDLENGTH - 1);
    uuid[UUIDLENGTH - 1] = '\0';

    fatfs_setDeploymentId(uuid);
    cli_append(&pcWriteBuffer, &xWriteBufferLen, "Deployment ID set to %s", uuid);
    return pdFALSE;
}

static BaseType_t prvGetdid(char *pcWriteBuffer, size_t xWriteBufferLen, const char *pcCommandString) {
    (void)pcCommandString;
    char uuid[UUIDLENGTH];
    fatfs_getDeploymentId(uuid, sizeof(uuid));
    cli_append(&pcWriteBuffer, &xWriteBufferLen, "Deployment ID: %s\n", uuid);
    return pdFALSE;
}
```

#### Registration (in `vRegisterCLICommands()`, alongside GPS commands)

```c
FreeRTOS_CLIRegisterCommand(&xSetDid);
FreeRTOS_CLIRegisterCommand(&xGetDid);
```

---

## Backward compatibility and migration path

| Scenario | Behaviour |
|----------|-----------|
| Old CONFIG.TXT (chunks only, no `I ` line) | `getDeploymentId` falls back to chunk reconstruction — no change |
| New CONFIG.TXT (`I ` line present) | String form used directly; chunks still loaded but ignored for getter |
| Both forms present | String form takes precedence |

The `OP_PARAMETER_DEPLOYMENT_ID_CHUNK_1-8` entries and the chunk reconstruction code are
retained unchanged. Once the new string form is fully tested on both embedded firmware and
app sides, these can be removed in a later cleanup.

---

## Files to change

| File | Change |
|------|--------|
| `fatfs_task.c` | Add `deployment_id_string[]`; update `load_configuration()`, `save_configuration()`, `fatfs_getDeploymentId()`; add `fatfs_setDeploymentId()` |
| `fatfs_task.h` | Add declaration of `fatfs_setDeploymentId()` |
| `CLI-commands.c` | Add prototypes, command structs, implementations, and registration for `setdid`/`getdid` |

`image_task.c` requires **no changes** — it already calls `fatfs_getDeploymentId()` which will
return the correct UUID regardless of which storage form was used.

---

## Notes

- The referenced `FIRMWARE_DEPLOYMENT_ID_SPEC.md` does not currently exist in the `doc/`
  directory — it may be worth creating it once this refactor is done, or the comment in
  `fatfs_task.c` could simply be removed.
- `UUIDLENGTH` is defined as 37 in `fatfs_task.h` (36 chars + null), which is correct for
  the standard UUID format `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`.
- The `setdid` command does not need underscore-to-space substitution (unlike `setgps`)
  since a UUID string contains no spaces.
