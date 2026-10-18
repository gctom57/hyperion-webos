#pragma once

// Tightens the permissions of the service's own install directory. The
// webOS installer leaves it world-writable (dir 0777, files 0666), so any
// process on the TV could replace the binary or a backend library that we
// then run as root.
//
// Directory and executables become 0755, config files 0600, everything else
// 0644, all owned by root (when running as root). Symlinks and
// subdirectories are left alone.
// Returns the number of entries changed, or -1 if dir can't be opened.
int lockdown_dir(const char* dir);
