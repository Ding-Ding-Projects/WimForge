# Portable project bundles

WimForge project saves use a single `.wimforge` file. A save is not just a
copy of `project.json`: it contains the complete project working tree and
hidden `.git` directory—including its contextual action-history journal—plus
the separate notification-history repository. Branches, tags, refs, reflogs,
objects, local Git configuration, hooks, and undo/redo commits therefore
survive export and import byte-for-byte.

The bundle can also carry ordinary supporting files outside those repositories.
Each repository has a stable role such as `project` or `notifications`; the
core format can also carry additional repository roles. Paths inside a bundle
are relative and use `/` separators.

## Version 1 container

Version 1 is a deliberately small streaming format implemented with public Qt
6.8 Core APIs. It has no ZIP implementation, private Qt dependency, shell
archive command, or temporary unpacker dependency.

| Field | Encoding |
| --- | --- |
| Magic | 16 bytes: `WIMFORGE-BUNDLE` followed by `0x1a` |
| Format version | unsigned 32-bit big-endian integer |
| Flags | unsigned 32-bit big-endian integer; zero in version 1 |
| Manifest size | unsigned 64-bit big-endian integer |
| Payload size | unsigned 64-bit big-endian integer |
| Manifest digest | 32-byte SHA-256 |
| Manifest | compact UTF-8 JSON |
| Payload | raw file bytes in manifest order |

Large sizes are decimal strings in JSON so they are never rounded through a
JSON floating-point number. The manifest records:

- format identifier and version;
- creation time and payload byte count;
- repository roles and their relative roots;
- standalone file paths;
- every file and directory, including hidden `.git` content;
- file payload offset, size, SHA-256, Qt permissions, and modification time.

The payload is intentionally uncompressed. This keeps recovery simple,
streaming, inspectable, deterministic, and independent of a platform archive
tool. The outer installer or transport may compress a `.wimforge` file if
needed.

## Export guarantees

`ProjectBundle::exportToFile()` writes with `QSaveFile`, so an interrupted
export does not replace a previous save with a partial file. It walks hidden and
system entries, hashes every file before writing the manifest, then hashes the
bytes again while streaming the payload. If a listed file changes, disappears,
or becomes unreadable during export, the save is cancelled.

The application should briefly pause writers to the included repositories while
export runs. Filesystem APIs cannot create a transaction spanning several
independent Git repositories; pausing writers ensures that all repositories
represent the same application moment.

Repository sources must contain an ordinary, local `.git` directory. Linked
worktrees whose `.git` is a pointer file are refused because exporting that file
alone would silently omit the real object database.

## Safe import and recovery

`ProjectBundle::importFromFile()` validates the header, exact total file length,
manifest SHA-256, schema, entry limits, payload ranges, and every file SHA-256.
It extracts only into a random sibling staging directory. The destination is
not visible until every byte has passed validation.

After validation, the staging directory is renamed into place on the same
volume. Existing destinations are refused by default. With explicit overwrite:

1. the old destination is renamed to a random safety backup;
2. validated staging is renamed to the requested destination;
3. promotion failure rolls the backup back;
4. after success the backup is removed, or its path is returned in
   `retainedBackupPath` if cleanup was blocked.

This means a corrupt or cancelled import never partially edits the active
project.

## Path and input defenses

Before touching staging, the importer rejects absolute paths, `.`/`..`, empty
segments, backslashes, Windows device names, alternate data-stream syntax,
control characters, trailing dots/spaces, case-insensitive collisions, file as
an ancestor of another entry, overlapping repository roots, and standalone
files placed under repository roots. Every declared repository must contain a
directory entry for its complete `.git` tree.

Exports and imports reject symbolic links and, on Windows, junctions. The
importer also rechecks canonical parent containment before each file write.
Configurable limits cap manifest size, entry count, individual file size, and
total payload size. Unknown versions and non-zero header flags fail closed.

Version 1 preserves ordinary file bytes and Git topology exactly. It does not
claim to preserve NTFS ACLs, alternate data streams, sparse-file allocation,
or extended attributes; none are required for Git object, ref, branch, tag, or
commit fidelity.

## Core API

```cpp
QList<ProjectBundleRepository> repositories = {
    {ProjectBundle::ProjectRepositoryRole,
     projectDirectory,
     QStringLiteral("repos/project")},
    {ProjectBundle::NotificationRepositoryRole,
     notificationDirectory,
     QStringLiteral("repos/notifications")},
};

ProjectBundle::exportToFile(savePath, repositories, supportingFiles, &error);
auto restored = ProjectBundle::importFromFile(savePath, destination, {}, &error);
```

The returned `repositoryPaths` map resolves each role to its restored local
directory, so the project, contextual history popover, and notification center
can reconnect to their original Git histories immediately.
