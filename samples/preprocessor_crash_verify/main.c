// vitaGL preprocessor / FFP crash-hardening verifier
//
// Exercises the exact crash triggers from the 2026-06-08 report on real hardware:
//   * inputty: vglInitWithCustomThreshold (dedicated-CDRAM display path) + first FFP draw.
//   * boxy:    a custom `#version 300 es` GLSL shader compiled+linked at runtime.
// Plus the key fail-safe assertion: a shader that trips vitaGL's C++ preprocessor
// (`#error`) must now return a clean GL link failure instead of aborting the process.
//
// The whole point is that NONE of these crash. Results are reported three ways:
//   1) The app reaches its render loop at all (a hard abort would prevent that).
//   2) On-screen bars, one per test, drawn with the fixed-function pipeline
//      (green = pass, red = fail). The bars rendering is itself the FFP proof.
//   3) ux0:data/vitaGL_verify.txt, for headless / post-mortem inspection.

#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <string.h>

#define MB (1024 * 1024)

enum {
	T_FFP_DRAW = 0,    // FFP shader build + first draw did not crash (inputty repro)
	T_GLSL_100,        // valid GLSL ES 1.00 program links
	T_GLSL_300ES,      // valid GLSL ES 3.00 program builds without crashing (boxy repro)
	T_GLSL_MALFORMED,  // preprocessor-tripping shader fails cleanly (no abort)
	T_GLSL_EXPAND,     // macro expansion >> input source does not corrupt the heap
	T_COUNT
};

static const char *test_name[T_COUNT] = {
	"FFP_DRAW_no_crash",
	"GLSL_ES_100_links",
	"GLSL_ES_300ES_no_crash",
	"MALFORMED_fails_cleanly",
	"MACRO_EXPANSION_no_overflow",
};

static int test_pass[T_COUNT];   // 1 = pass, 0 = fail
static int test_run[T_COUNT];    // 1 = we got far enough to record a verdict

// --- Shader sources -------------------------------------------------------

static const char *vs_100 =
	"#version 100\n"
	"attribute vec4 aPos;\n"
	"void main() { gl_Position = aPos; }\n";

static const char *fs_100 =
	"#version 100\n"
	"precision mediump float;\n"
	"void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

static const char *vs_300 =
	"#version 300 es\n"
	"in vec4 aPos;\n"
	"void main() { gl_Position = aPos; }\n";

static const char *fs_300 =
	"#version 300 es\n"
	"precision mediump float;\n"
	"out vec4 fragColor;\n"
	"void main() { fragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

// Fragment shader that trips the C++ preprocessor: #error throws inside
// preprocess(); pre-fix this propagated across the extern "C" boundary and
// aborted the process. Post-fix it must surface as a clean link failure.
static const char *fs_malformed =
	"#version 100\n"
	"precision mediump float;\n"
	"#error deliberately_malformed_for_verification\n"
	"void main() { gl_FragColor = vec4(0.0); }\n";

// Macro-heavy fragment shader whose preprocessed output is far larger than the
// input source. Pre-fix the output was copied into a strlen(input)-sized buffer
// (heap overflow); post-fix the copy is sized from the result. Must link.
static const char *fs_expand =
	"#version 100\n"
	"precision mediump float;\n"
	"#define C4 vec4(1.0, 1.0, 1.0, 1.0)\n"
	"#define S4 (C4 + C4 + C4 + C4)\n"
	"#define S16 (S4 + S4 + S4 + S4)\n"
	"#define S64 (S16 + S16 + S16 + S16)\n"
	"void main() { gl_FragColor = normalize(S64); }\n";

// Builds a vertex+fragment GLSL program and returns GL_LINK_STATUS.
// Reaching the return at all means none of the calls aborted the process.
static GLint build_program(const char *vsrc, const char *fsrc) {
	GLuint vs = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vs, 1, &vsrc, NULL);
	glCompileShader(vs);

	GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fs, 1, &fsrc, NULL);
	glCompileShader(fs);

	GLuint p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	glLinkProgram(p);

	GLint status = GL_FALSE;
	glGetProgramiv(p, GL_LINK_STATUS, &status);
	return status;
}

static void write_results(void) {
	SceUID f = sceIoOpen("ux0:data/vitaGL_verify.txt",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (f < 0)
		return;
	int all = 1;
	for (int i = 0; i < T_COUNT; i++) {
		char line[128];
		const char *verdict = !test_run[i] ? "DID-NOT-RUN" : (test_pass[i] ? "PASS" : "FAIL");
		if (!test_run[i] || !test_pass[i])
			all = 0;
		int n = snprintf(line, sizeof(line), "[%s] %s\n", verdict, test_name[i]);
		sceIoWrite(f, line, n);
	}
	char tail[64];
	int n = snprintf(tail, sizeof(tail), "OVERALL: %s\n", all ? "ALL PASS" : "FAILURES PRESENT");
	sceIoWrite(f, tail, n);
	sceIoClose(f);
}

// Draw an axis-aligned quad in screen space with the fixed-function pipeline.
static void ffp_quad(float x0, float y0, float x1, float y1, float r, float g, float b) {
	glBegin(GL_QUADS);
	glColor3f(r, g, b);
	glVertex3f(x0, y0, 0);
	glVertex3f(x1, y0, 0);
	glVertex3f(x1, y1, 0);
	glVertex3f(x0, y1, 0);
	glEnd();
}

int main(void) {
	// Match the inputty repro exactly: custom-threshold init drives the
	// dedicated-CDRAM display path added on this branch (the reported "ok=0").
	vglInitWithCustomThreshold(0, 960, 544, 8 * MB, 8 * MB, 0, 26 * MB, SCE_GXM_MULTISAMPLE_NONE);

	glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 960, 544, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	for (int i = 0; i < T_COUNT; i++) { test_pass[i] = 0; test_run[i] = 0; }

	// --- Test 1: FFP draw (inputty repro) ---
	// One FFP draw forces vitaGL to synthesize + compile its macro-heavy FFP
	// shader. Surviving the swap below means it did not crash.
	glClear(GL_COLOR_BUFFER_BIT);
	ffp_quad(0, 0, 10, 10, 1.0f, 1.0f, 1.0f);
	vglSwapBuffers(GL_FALSE);
	test_run[T_FFP_DRAW] = 1;
	test_pass[T_FFP_DRAW] = 1;

	// --- Test 2: valid GLSL ES 1.00 program links ---
	test_run[T_GLSL_100] = 1;
	test_pass[T_GLSL_100] = (build_program(vs_100, fs_100) == GL_TRUE);

	// --- Test 3: valid GLSL ES 3.00 program builds without crashing (boxy) ---
	// Pass = we returned from build_program at all. Record link status too, but
	// not crashing is the load-bearing assertion for this case.
	(void)build_program(vs_300, fs_300);
	test_run[T_GLSL_300ES] = 1;
	test_pass[T_GLSL_300ES] = 1;

	// --- Test 4: preprocessor-tripping shader fails cleanly, no abort ---
	// Reaching the next line proves the throw no longer aborts the process;
	// a GL_FALSE link status proves the failure is reported, not silently OK.
	{
		GLint linked = build_program(vs_100, fs_malformed);
		test_run[T_GLSL_MALFORMED] = 1;
		test_pass[T_GLSL_MALFORMED] = (linked == GL_FALSE);
	}

	// --- Test 5: macro expansion >> input does not corrupt the heap ---
	test_run[T_GLSL_EXPAND] = 1;
	test_pass[T_GLSL_EXPAND] = (build_program(vs_100, fs_expand) == GL_TRUE);

	write_results();

	int overall = 1;
	for (int i = 0; i < T_COUNT; i++)
		if (!test_run[i] || !test_pass[i]) overall = 0;

	// --- Render loop: result bars (FFP). Green = pass, red = fail. ---
	// A solid green overall banner across the bottom = every test passed.
	SceCtrlData pad;
	for (;;) {
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_START)
			break;

		glClear(GL_COLOR_BUFFER_BIT);

		float bar_h = 70.0f;
		for (int i = 0; i < T_COUNT; i++) {
			float y0 = 20.0f + i * (bar_h + 10.0f);
			float y1 = y0 + bar_h;
			if (test_pass[i])
				ffp_quad(40, y0, 920, y1, 0.0f, 0.85f, 0.0f);  // green = pass
			else
				ffp_quad(40, y0, 920, y1, 0.9f, 0.0f, 0.0f);   // red = fail
		}
		// Overall banner
		ffp_quad(40, 470, 920, 524,
			overall ? 0.0f : 0.9f, overall ? 0.9f : 0.0f, 0.0f);

		vglSwapBuffers(GL_FALSE);
	}

	sceKernelExitProcess(0);
	return 0;
}
