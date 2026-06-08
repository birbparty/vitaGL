// Preprocess shader source `fname` (the source text itself). Allocates an exactly-sized
// NUL-terminated result into *output (caller frees with vgl_free). Returns bytes written
// (excl. NUL), or -1 on error (with *output set to NULL).
int glsl_preprocess(const char *mode, const char *fname, char **output);
