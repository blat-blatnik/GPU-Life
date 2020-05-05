#ifdef BENCHMARK
#define NDEBUG
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "glad.h"
#include "glfw3.h"
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* run on a dedicated GPU if avaliable https://stackoverflow.com/a/39047129 */
#ifdef _MSC_VER
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

GLFWwindow *window;
int windowWidth, windowHeight;
double mouseX, mouseY;
int pressedButton = -1;
int numCellsX, numCellsY;
float scale = 1.0f;
float scaleX = 1.0f;
float scaleY = 1.0f;
float offsetX = 0.0f;
float offsetY = 0.0f;
GLboolean cellBorderIsOn = GL_TRUE;
GLboolean vsyncIsOn = GL_TRUE;
GLboolean isRunning;
int updatesPerFrame = 1;
int framesPerUpdate = 1;
int generation;
char *patternName;
float backgroundColor = 0.0f;
float deadColor       = 0.1f;
float aliveColor      = 1.0f;
GLint maxTextureSize;
GLuint cellsRead;
GLuint cellsWrite;
GLuint cellsReadFramebuffer;
GLuint cellsWriteFramebuffer;
GLuint renderProgram;
GLuint updateProgram;
GLint uniformScale;
GLint uniformOffset;
GLint uniformBorderSize;
GLint uniformBackgroundColor;
GLint uniformDeadColor;
GLint uniformAliveColor;

/* the vertex shader is shared between the render and update shaders */
const char *vertShaderSource =
	"#version 130\n"
	"in vec2 position;\n"
	"out vec2 uv;\n"
	"uniform vec2 offset = vec2(0.0);\n"
	"uniform vec2 scale = vec2(1.0);\n"
	"void main() {\n"
	"	uv = 0.5 * position + 0.5;\n"
	"	uv = uv * scale + offset;\n"
	"	gl_Position = vec4(position, 0.0, 1.0);\n"
	"}";

/* this shader uses some tricks to perform sub-pixel rendering when zoomed-in close so the cell
   border doesn't appear jittery, and it also does super-pixel rendering when zoomed-out in a
   very naive way - it just samples every single cell that the fragment covers. the way this is
   done is a bit complicated because we try to avoid texture fetches as much as possible. */
const char *renderShaderSource = 
	"#version 130\n"
	"in vec2 uv;\n"
	"out vec3 color;\n"
	"uniform usampler2D cells;\n"
	"uniform float borderSize = 0.1;\n"
	"uniform float backgroundColor;\n"
	"uniform float deadColor;\n"
	"uniform float aliveColor;\n"
	"void main() {\n"
	"	ivec2 numCells = textureSize(cells, 0) * ivec2(1, 32);\n"
	"	vec2 fpos = uv * vec2(numCells);\n"
	"	vec2 delta = abs(vec2(dFdx(fpos.x), dFdy(fpos.y)));\n"
	"	if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {\n"
	"		color = vec3(backgroundColor);\n"
	"		return;\n"
	"	}\n"
	"	ivec2 pmin = ivec2(fpos - 0.5 * delta);\n"
	"	ivec2 pmax = ivec2(fpos + 0.5 * delta);\n"
	"	pmin = clamp(pmin, ivec2(0), numCells - 1);\n"
	"	pmax = clamp(pmax, ivec2(0), numCells - 1);\n"
	"	uint accumulator = 0u;\n"
	"	int yadvance;\n"
	"	for (int y = pmin.y; y <= pmax.y; y += yadvance) {\n"
	"		int ymin = y % 32;"
	"		int ymax = min(31, pmax.y + ymin - y);"
	"		int lshift = 31 - ymax;"
	"		int rshift = ymin + lshift;"
	"		for (int x = pmin.x; x <= pmax.x; ++x) {\n"
	"			uint cellColumn = texelFetch(cells, ivec2(x, y / 32), 0).x;"
	"			accumulator |= (cellColumn << lshift) >> rshift;"
	"		}\n"
	"		yadvance = 1 + ymax - ymin;"
	"	}\n"
	"	color = vec3(accumulator != 0u ? aliveColor : deadColor);\n"
	"	if (delta.x < 0.2 && delta.y < 0.2) {\n"
	"		vec2 fragMin = fpos - 0.5 * delta;\n"
	"		vec2 fragMax = fpos + 0.5 * delta;\n"
	"		vec2 cellMin = floor(fpos) + borderSize;\n"
	"		vec2 cellMax = ceil (fpos) - borderSize;\n"
	"		if (any(lessThan(fragMin, cellMin)) || any(greaterThan(fragMax, cellMax))) {\n"
	"			vec2 d = max(min(fragMax, cellMax) - max(fragMin, cellMin), 0.0);\n"
	"			float fragSize = (fragMax.x - fragMin.x) * (fragMax.y - fragMin.y);\n"
	"			float overlap = d.x * d.y / fragSize;\n"
	"			color = mix(vec3(deadColor), color, clamp(overlap, 0.0, 1.0));\n"
	"		}\n"
	"	}\n"
	"}";

/* this shader looks quite complex with all the bitwise stuff going on
   but all it does is efficiently sum up the number of neighboring cells
   for each of the 32 cells in a column at the same time - using bitwise
   instructions. there might be more efficient ways to do this, however
   that doesn't matter since this shader is entirely memory bound */
const char *updateShaderSource = 
	"#version 130\n"
	"in vec2 uv;\n"
	"out uint newCells;\n"
	"uniform usampler2D cells;\n"
	"void main() {\n"
	"	uint n00 = textureOffset(cells, uv, ivec2(-1,-1)).x;\n"
	"	uint n10 = textureOffset(cells, uv, ivec2( 0,-1)).x;\n"
	"	uint n20 = textureOffset(cells, uv, ivec2(+1,-1)).x;\n"
	"	uint n01 = textureOffset(cells, uv, ivec2(-1, 0)).x;\n"
	"	uint n11 = textureOffset(cells, uv, ivec2( 0, 0)).x;\n"
	"	uint n21 = textureOffset(cells, uv, ivec2(+1, 0)).x;\n"
	"	uint n02 = textureOffset(cells, uv, ivec2(-1,+1)).x;\n"
	"	uint n12 = textureOffset(cells, uv, ivec2( 0,+1)).x;\n"
	"	uint n22 = textureOffset(cells, uv, ivec2(+1,+1)).x;\n"
	"	uint sumLo0 = n00 ^ n10 ^ n20;\n"
	"	uint sumLo1 = n01 ^ n11 ^ n21;\n"
	"	uint sumLo2 = n02 ^ n12 ^ n22;\n"
	"	uint sumHi0 = (n00 & n10) | (n10 & n20) | (n20 & n00);\n"
	"	uint sumHi1 = (n01 & n11) | (n11 & n21) | (n21 & n01);\n"
	"	uint sumHi2 = (n02 & n12) | (n12 & n22) | (n22 & n02);\n"
	"	uint x0 = (sumLo1 >> 1) | (sumLo2 << 31);\n"
	"	uint y0 = (sumHi1 >> 1) | (sumHi2 << 31);\n"
	"	uint x1 = sumLo1;\n"
	"	uint y1 = sumHi1;\n"
	"	uint x2 = (sumLo1 << 1) | (sumLo0 >> 31);\n"
	"	uint y2 = (sumHi1 << 1) | (sumHi0 >> 31);\n"
	"	uint xc = (x0 & x1) | (x1 & x2) | (x2 & x0);\n"
	"	uint c = x0 ^ x1 ^ x2;\n"
	"	uint b = y0 ^ y1 ^ y2 ^ xc;\n"
	"	uint a = ((y0 & (y1 | xc)) | (y1 & (y2 | xc)) | (y2 & (y0 | xc))) & ~(y0 & y1 & y2 & xc);\n"
	"	newCells = (~a & b & c) | (n11 & a & ~b & ~c);\n"
	"}";

#ifndef NDEBUG
#define glCheckErrors()\
	do {\
		GLenum code = glGetError();\
		if (code != GL_NO_ERROR) {\
			const char *desc;\
			switch (code) {\
				case GL_INVALID_ENUM:      desc = "invalid enum";      break;\
				case GL_INVALID_VALUE:     desc = "invalid value";     break;\
				case GL_INVALID_OPERATION: desc = "invalid operation"; break;\
				case GL_OUT_OF_MEMORY:     desc = "out of memory";     break;\
				case GL_INVALID_FRAMEBUFFER_OPERATION: desc = "invalid framebuffer operation"; break;\
				default: desc = "unknown error"; break;\
			}\
			fprintf(stderr, "OpenGL ERROR %s in %s:%d\n", desc, __FILE__, (int)__LINE__);\
		}\
	} while (0)
#else
#define glCheckErrors() do {} while(0)
#endif /* !NDEBUG */

GLuint compileShader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type);
	if (!shader) {
		fprintf(stderr, "ERROR: OpenGL failed to allocate shader .. aborting\n");
		abort();
	}

	const GLchar *src = (const GLchar *)source;
	glShaderSource(shader, 1, &src, NULL);
	glCompileShader(shader);

	GLint compileOk;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compileOk);
	if (!compileOk) {
		fprintf(stderr, "ERROR: GLSL didnt compile .. ");
	#ifndef NDEBUG
		GLint logLength;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
		char *log = (char *)malloc((size_t)logLength);
		glGetShaderInfoLog(shader, logLength, NULL, (GLchar *)log);
		fprintf(stderr, "\n%s\n", log);
		free(log);
	#endif
		fprintf(stderr, "aborting\n");
		abort();
	}

	return shader;
}

GLuint linkShaderProgram(const GLuint *shaders, int numShaders) {
	GLuint program = glCreateProgram();
	if (!program) {
		fprintf(stderr, "ERROR: OpenGL failed to allocate shader program .. aborting\n");
		abort();
	}

	for (int i = 0; i < numShaders; ++i)
		glAttachShader(program, shaders[i]);

	glLinkProgram(program);

	for (int i = 0; i < numShaders; ++i)
		glDetachShader(program, shaders[i]);

	GLint linkOk;
	glGetProgramiv(program, GL_LINK_STATUS, &linkOk);
	if (!linkOk) {
		fprintf(stderr, "ERROR: GLSL didnt link .. ");
	#ifndef NDEBUG
		GLint logLength;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
		char *log = (char *)malloc((size_t)logLength);
		glGetProgramInfoLog(program, logLength, NULL, (GLchar *)log);
		fprintf(stderr, "\n%s\n", log);
		free(log);
	#endif
		fprintf(stderr, "aborting\n");
		abort();
	}

	return program;
}

GLuint createTexture(const void *pixels, int width, int height, GLenum format, GLenum internalFormat) {
	if (width > maxTextureSize || height > maxTextureSize) {
		fprintf(stderr, "ERROR: tried to allocate %d x %d texture but maximum size is %d x %d\n", width, height, maxTextureSize, maxTextureSize);
		width = width > maxTextureSize ? maxTextureSize : width;
		height = height > maxTextureSize ? maxTextureSize : height;
	}

	GLuint texture;
	glGenTextures(1, &texture);
	if (!texture) {
		fprintf(stderr, "ERROR: OpenGL failed to allocate %d x %d texture .. aborting\n", width, height);
		abort();
	}

	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internalFormat,
		(GLsizei)width, (GLsizei)height, 0, format, GL_UNSIGNED_BYTE, pixels);

	return texture;
}

GLuint createFramebuffer(GLuint texture) {
	GLuint framebuffer;
	glGenFramebuffers(1, &framebuffer);
	if (!framebuffer) {
		fprintf(stderr, "ERROR: OpenGL failed to allocate framebuffer .. aborting\n");
		abort();
	}

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "WARNING: framebuffer is not complete\n");

	return framebuffer;
}

void swap(GLuint *a, GLuint *b) {
	GLuint temp = *a;
	*a = *b;
	*b = temp;
}

int ceilMultipleOf32(int x) {
	if (x % 32 == 0)
		return x;
	return (x + 32) & ~31;
}

void findFilePartOfPath(const char *path, size_t *start, size_t *end) {
	*start = 0;
	*end = 0;
	size_t i;
	for (i = 0; path[i] != 0; ++i) {
		char c = path[i];
		if (c == '/' || c == '\\' || c == ':')
			*start = i + 1;
		else if (c == '.')
			*end = i;
	}

	if (*end == 0)
		*end = i;
}

void setPatternName(const char *name) {
	size_t start, end;
	findFilePartOfPath(name, &start, &end);
	size_t len = end - start;
	patternName = (char *)realloc(patternName, len + 1);
	if (patternName) {
		memcpy(patternName, name + start, len);
		patternName[len] = 0;
	}
}

GLboolean keyModsArePressed(void) {
	return
		glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
		glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS ||
		glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
		glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS ||
		glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
		glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
}

void updateCells(void) {
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cellsWriteFramebuffer);
	glViewport(0, 0, numCellsX, numCellsY / 32);
	glUseProgram(updateProgram);
	glBindTexture(GL_TEXTURE_2D, cellsRead);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	swap(&cellsRead, &cellsWrite);
	swap(&cellsReadFramebuffer, &cellsWriteFramebuffer);
	++generation;
	glCheckErrors();
}

void renderCells(void) {
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, windowWidth, windowHeight);
	glUseProgram(renderProgram);
	glBindTexture(GL_TEXTURE_2D, cellsRead);
	glUniform2f(uniformScale, scale * scaleX, scale * scaleY);
	glUniform2f(uniformOffset, offsetX, offsetY);
	glUniform1f(uniformBorderSize, cellBorderIsOn ? 0.1f : -0.1f);
	glUniform1f(uniformBackgroundColor, backgroundColor);
	glUniform1f(uniformDeadColor, deadColor);
	glUniform1f(uniformAliveColor, aliveColor);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glCheckErrors();
}

void centerCellsOnScreen(void) {
	scale = 1.0f;
	scaleX = 1.0f;
	scaleY = 1.0f;
	offsetX = 0.0f;
	offsetY = 0.0f;

	float windowAspect = (float)windowWidth / windowHeight;
	float cellsAspect = (float)numCellsX / numCellsY;

	/* we want:
		(scaleX * numCellsX) / (scaleY * numCellsY) == windowWidth / windowHeight
		scaleX * 0.5 + offsetX == 0.5
		scaleY * 0.5 + offsetY == 0.5
	*/

	if (cellsAspect > windowAspect) {
		scaleY = scaleX * numCellsX / (windowAspect * numCellsY);
		offsetY = 0.5f - 0.5f * scaleY;
	} else if (cellsAspect < windowAspect) {
		scaleX = windowAspect * scaleY * numCellsY / numCellsX;
		offsetX = 0.5f - 0.5f * scaleX;
	}
}

void setCells(uint8_t *cells, int width, int height) {	
	int w = ceilMultipleOf32(width);
	int h = ceilMultipleOf32(height);
	
	if (w < 1 || h < 1) {
		fprintf(stderr, "ERROR: invalid pattern size %d x %d .. ignoring\n", w, h);
		return;
	}

	if (w > maxTextureSize || h > maxTextureSize) {
		fprintf(stderr, "ERROR: pattern size %d x %d is larger than maximum %d x %d .. ignoring\n",
			w, h, maxTextureSize, maxTextureSize);
		return;
	}

	numCellsX = w;
	numCellsY = h;
	int numCellColumnsY = numCellsY / 32;
	size_t numCellColumns = (size_t)numCellsX * (size_t)numCellsY;
	uint32_t *cellColumns = (uint32_t *)calloc(numCellColumns, sizeof(uint32_t));

	for (int y = 0; y < height; ++y) {
		uint32_t *cellColumn = &cellColumns[(y / 32) * numCellsX];
		int cellColumnBit = y % 32;
		const uint8_t *cellRow = &cells[y * width];
		for (int x = 0; x < width; ++x) {
			if (cellRow[x] != 0) {
				cellColumn[x] |= (1 << cellColumnBit);
			}
		}
	}

	glBindTexture(GL_TEXTURE_2D, cellsWrite);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
		(GLsizei)numCellsX, (GLsizei)numCellColumnsY, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
	glBindTexture(GL_TEXTURE_2D, cellsRead);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI,
		(GLsizei)numCellsX, (GLsizei)numCellColumnsY, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, cellColumns);

	generation = 0;
	free(cellColumns);
	centerCellsOnScreen();
}

void clearCells() {
	setPatternName("unnamed pattern");
	glBindFramebuffer(GL_FRAMEBUFFER, cellsReadFramebuffer);
	/* this seems to work - even though the format is unsigned 
	   integer and not floating point.. should look into glClearBuffer */
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	generation = 0;
}

void onGlfwError(int code, const char *desc) {
	printf("GLFW error 0x%X: %s\n", code, desc);
}

void onFramebufferResized(GLFWwindow *window, int newWidth, int newHeight) {
	glViewport(0, 0, newWidth, newHeight);
	windowWidth = newWidth;
	windowHeight = newHeight;
	centerCellsOnScreen();
}

void onMouseButton(GLFWwindow *window, int button, int action, int mods) {
	if (action == GLFW_PRESS)
		pressedButton = button;
	else if (action == GLFW_RELEASE) {
		if (button == pressedButton)
			pressedButton = -1;
		return;
	}

	if (button != GLFW_MOUSE_BUTTON_LEFT && button != GLFW_MOUSE_BUTTON_RIGHT || keyModsArePressed())
		return;

	float mX = (float)(mouseX / windowWidth);
	float mY = (float)(mouseY / windowHeight);
	int x = (int)(numCellsX * (offsetX + mX * scaleX * scale));
	int y = (int)(numCellsY * (offsetY + mY * scaleY * scale));
	if (x >= 0 && x < numCellsX && y >= 0 && y < numCellsY) {

		uint8_t value = 0;
		if (button == GLFW_MOUSE_BUTTON_LEFT) {
			value = 0xFF;
		}

		uint32_t cellColumn;
		glBindFramebuffer(GL_FRAMEBUFFER, cellsReadFramebuffer);
		glReadPixels(x, y / 32, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &cellColumn);
		if (value)
			cellColumn |= (1 << (y & 31));
		else
			cellColumn &= ~(1 << (y & 31));
		glBindTexture(GL_TEXTURE_2D, cellsRead);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y / 32, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &cellColumn);
	}
}

void onMouseMove(GLFWwindow *window, double newX, double newY) {
	newY = (double)windowHeight - newY;
	double oldX = mouseX;
	double oldY = mouseY;
	double deltaX = newX - oldX;
	double deltaY = newY - oldY;
	mouseX = newX;
	mouseY = newY;

	if (pressedButton != GLFW_MOUSE_BUTTON_LEFT && pressedButton != GLFW_MOUSE_BUTTON_RIGHT && pressedButton != GLFW_MOUSE_BUTTON_MIDDLE)
		return;

	if (pressedButton == GLFW_MOUSE_BUTTON_MIDDLE || keyModsArePressed()) {
		offsetX -= scale * scaleX * (float)(deltaX / windowWidth);
		offsetY -= scale * scaleY * (float)(deltaY / windowHeight);
		return;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, cellsReadFramebuffer);
	glBindTexture(GL_TEXTURE_2D, cellsRead);

	uint8_t value = 0;
	if (pressedButton == GLFW_MOUSE_BUTTON_LEFT)
		value = 0xFF;

	/* Bresenham's line algorithm */
	float mX0 = (float)(oldX / windowWidth);
	float mY0 = (float)(oldY / windowHeight);
	float mX1 = (float)(newX / windowWidth);
	float mY1 = (float)(newY / windowHeight);
	int x = (int)(numCellsX * (offsetX + mX0 * scaleX * scale));
	int y = (int)(numCellsY * (offsetY + mY0 * scaleY * scale));
	int xend = (int)(numCellsX * (offsetX + mX1 * scaleX * scale));
	int yend = (int)(numCellsY * (offsetY + mY1 * scaleY * scale));

	int dX = +abs(xend - x);
	int dY = -abs(yend - y);
	int signX = x < xend ? +1 : -1;
	int signY = y < yend ? +1 : -1;
	int error = dX + dY;

	for (;;) {
		if (x >= 0 && x < numCellsX && y >= 0 && y < numCellsY) {
			/* this can actually be pretty slow for long lines on huge cell fields
			   it would probably be much faster to "render" a line primitive instead
			   of setting each pixel manually, but that is quite complicated becaue
			   of the way that the cells are stored in a single bit.. */
			uint32_t cellColumn;
			glReadPixels(x, y / 32, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &cellColumn);
			if (value)
				cellColumn |= (1 << (y & 31));
			else
				cellColumn &= ~(1 << (y & 31));
			glTexSubImage2D(GL_TEXTURE_2D, 0, x, y / 32, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &cellColumn);
		}

		if (x == xend && y == yend)
			break;

		int error2 = 2 * error;
		if (error2 >= dY) {
			error += dY;
			x += signX;
		}
		if (error2 <= dX) {
			error += dX;
			y += signY;
		}
	}
}

void onMouseWheel(GLFWwindow *window, double dX, double dY) {
	if (keyModsArePressed()) {
		float mX = (float)(mouseX / windowWidth);
		float mY = (float)(mouseY / windowHeight);
		float centerX = offsetX + mX * scaleX * scale;
		float centerY = offsetY + mY * scaleY * scale;
		/* we get visible rendering glitches with very small scales so we need to clamp */
		if (dY > 0)
			scale = fmaxf(scale / 1.1f, 0.0001f);
		else if (dY < 0)
			scale = fminf(scale * 1.1f, 10.0f);
		offsetX = centerX - mX * scaleX * scale;
		offsetY = centerY - mY * scaleY * scale;
	} else {
		if (dY > 0) {
			if (framesPerUpdate > 1)
				framesPerUpdate /= 2;
			else
				updatesPerFrame *= 2;
		} else if (dY < 0) {
			if (updatesPerFrame > 1)
				updatesPerFrame /= 2;
			else
				framesPerUpdate *= 2;
		}
	}
}

void onKey(GLFWwindow *window, int key, int scancode, int action, int mods) {
	if (action != GLFW_PRESS && action != GLFW_REPEAT)
		return;

	switch (key) {
		case GLFW_KEY_ESCAPE: {
			GLFWmonitor *monitor = glfwGetWindowMonitor(window);
			if (monitor) {
				int width, height;
				glfwGetMonitorWorkarea(monitor, NULL, NULL, &width, &height);
				glfwSetWindowMonitor(window, NULL, (windowWidth - 1280) / 2, (height - 720) / 2, 1280, 720, 60);
			} 
			else 
				glfwSetWindowShouldClose(window, GLFW_TRUE);
		}break;
		case GLFW_KEY_ENTER:
		case GLFW_KEY_PAUSE:
			isRunning = !isRunning;
			break;
		case GLFW_KEY_C:
		case GLFW_KEY_HOME:
			centerCellsOnScreen();
			break;
		case GLFW_KEY_B:
			cellBorderIsOn = !cellBorderIsOn;
			break;
		case GLFW_KEY_V:
			vsyncIsOn = !vsyncIsOn;
			glfwSwapInterval(vsyncIsOn);
			break;
		case GLFW_KEY_EQUAL:
		case GLFW_KEY_KP_ADD:
			onMouseWheel(window, 0.0, 1.0);
			break;
		case GLFW_KEY_MINUS:
		case GLFW_KEY_KP_SUBTRACT:
			onMouseWheel(window, 0.0, -1.0);
			break;
		case GLFW_KEY_LEFT:
			offsetX -= scale * scaleX * 0.05f;
			break;
		case GLFW_KEY_RIGHT:
			offsetX += scale * scaleX * 0.05f;
			break;
		case GLFW_KEY_UP:
			offsetY += scale * scaleY * 0.05f;
			break;
		case GLFW_KEY_DOWN:
			offsetY -= scale * scaleY * 0.05f;
			break;
		case GLFW_KEY_L:
			backgroundColor = 0.9f;
			deadColor = 1.0f;
			aliveColor = 0.0f;
			break;
		case GLFW_KEY_D:
			backgroundColor = 0.0f;
			deadColor = 0.1f;
			aliveColor = 1.0f;
			break;
		case GLFW_KEY_SPACE:
		case GLFW_KEY_KP_ENTER:
		case GLFW_KEY_PERIOD:
		case GLFW_KEY_TAB:
		case GLFW_KEY_S:
			updateCells();
			break;
		case GLFW_KEY_F11:
		case GLFW_KEY_F: {
			GLFWmonitor *monitor = glfwGetWindowMonitor(window);
			GLFWmonitor *primaryMonitor = glfwGetPrimaryMonitor();
			int width, height;
			glfwGetMonitorWorkarea(primaryMonitor, NULL, NULL, &width, &height);
			if (!monitor)
				glfwSetWindowMonitor(window, primaryMonitor, 0, 0, width, height, 60);
			else
				glfwSetWindowMonitor(window, NULL, (width - 1280) / 2, (height - 720) / 2, 1280, 720, 60);
		} break;
		case GLFW_KEY_BACKSPACE:
		case GLFW_KEY_DELETE:
			clearCells();
			break;
		default: break;
	}
}

void onFileDragNDrop(GLFWwindow *window, int numFiles, const char **files) {

	const char *file = files[0];
	FILE *f = fopen(file, "rt");
	if (!f) {
		printf("couldnt open %s\n", file);
		return;
	}

	char ignored;
	int width, height;
	
	/* life 1.06 file (.life) */
	fseek(f, 0, SEEK_SET);
	if (1 == fscanf(f, " #Life 1.06%c ", &ignored)) {
		printf("loading %s .. ", file);
		int minX = 0;
		int maxX = 0;
		int minY = 0;
		int maxY = 0;
		int x, y;
		while (2 == fscanf(f, " %d %d ", &x, &y)) {
			if (x < minX) minX = x;
			if (x > maxX) maxX = x;
			if (y < minY) minY = y;
			if (y > maxY) maxY = y;
		}
		width = 1 + maxX - minX;
		height = 1 + maxY - minY;
		if (width < 1 && height < 1) {
			printf("invalid life 1.06 file\n");
			return;
		}
		if (width > maxTextureSize || height > maxTextureSize) {
			printf("%d x %d texture is larger than the maximum %d x %d\n", width, height, maxTextureSize, maxTextureSize);
			return;
		}

		uint8_t *cells = (uint8_t *)calloc((size_t)width * (size_t)height, 1);

		fseek(f, 0, SEEK_SET);
		fscanf(f, " #Life 1.06 ");
		while (2 == fscanf(f, " %d %d ", &x, &y)) {
			x -= minX;
			y -= minY;
			y = height - y - 1;
			cells[y * width + x] = 255;
		}

		setPatternName(file);
		setCells(cells, width, height);
		free(cells);
		printf("done\n");
		return;
	}

	/* RLE life file (.rle) */
	fseek(f, 0, SEEK_SET);
	char lineBuffer[1024];
	char *line;
	do {
		line = fgets(lineBuffer, sizeof(lineBuffer), f);
	} while (line && line[0] == '#');
	if (2 == sscanf(line, " x = %d , y = %d ", &width, &height)) {

		printf("loading %s .. ", file);

		if (width > maxTextureSize || height > maxTextureSize) {
			printf("%d x %d texture is larger than the maximum %d x %d\n", 
				width, height, maxTextureSize, maxTextureSize);
			return;
		}

		uint8_t *cells = (uint8_t *)calloc((size_t)width * (size_t)height, 1);
		int cursorX = 0;
		int cursorY = height - 1;
		for (;;) {
			
			int runCount = 1;
			char tag;

			if (2 == fscanf(f, " %d %c ", &runCount, &tag)) {
				if (tag == 'o') {
					memset(&cells[cursorY * width + cursorX], 255, runCount);
					cursorX += runCount;
				} else if (tag == 'b') {
					cursorX += runCount;
				} else if (tag == '$') {
					cursorY -= runCount;
					cursorX = 0;
				}
				continue;
			}
			
			if (1 == fscanf(f, " %c ", &tag)) {
				if (tag == '!')
					break;
				else if (tag == '$') {
					--cursorY;
					cursorX = 0;
				} else if (tag == 'o')
					cells[cursorY * width + cursorX++] = 255;
				else if (tag == 'b')
					++cursorX;
				continue;
			}
			
			return;
		}

		setPatternName(file);
		setCells(cells, width, height);
		free(cells);
		printf("done\n");
		return;
	}

	/* image file */
	fclose(f);
	int comp;
	if (stbi_info(file, &width, &height, &comp)) {

		stbi_set_flip_vertically_on_load(1);
		stbi_uc *cells = stbi_load(file, &width, &height, &comp, STBI_grey);
		if (!cells) {
			printf("couldnt load %s: %s\n", file, stbi_failure_reason());
			return;
		}

		printf("loading %s .. ", file);

		if (width > maxTextureSize || height > maxTextureSize) {
			printf("%d x %d texture is larger than the maximum %d x %d\n", 
				width, height, maxTextureSize, maxTextureSize);
			return;
		}

		int64_t size = (int64_t)width * (int64_t)height;
		for (int64_t i = 0; i < size; ++i) {
			if (cells[i] > 127)
				cells[i] = 0;
			else
				cells[i] = 255;
		}

		setPatternName(file);
		setCells(cells, width, height);
		stbi_image_free(cells);
		printf("done\n");
		return;
	}

	printf("unknown file format %s\n", file);
}

int main(void) {
	glfwSetErrorCallback(onGlfwError);
	int glfwOk = glfwInit();
	if (!glfwOk) {
		fprintf(stderr, "ERROR: GLFW failed to initialize .. aborting\n");
		abort();
	}

	glfwWindowHint(GLFW_DEPTH_BITS, 0);
	glfwWindowHint(GLFW_STENCIL_BITS, 0);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	/* glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); <-- only for OpenGL 3.2+ */
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

	window = glfwCreateWindow(1280, 720, "GPU Life", NULL, NULL);
	if (!window) {
		fprintf(stderr, "ERROR: GLFW failed to open window .. aborting\n");
		abort();
	}

	glfwMakeContextCurrent(window);

	int gladOk = gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);
	if (!gladOk) {
		fprintf(stderr, "ERROR: GLAD failed to load OpenGL functions .. aborting\n");
		abort();
	}

	printf("using OpenGL %s: %s\n",
		(const char *)glGetString(GL_VERSION),
		(const char *)glGetString(GL_RENDERER));

	if (GLVersion.major < 3) {
		fprintf(stderr, "ERROR: need at least OpenGL 3.0 to run .. aborting\n");
		abort();
	}

	glfwSwapInterval(vsyncIsOn);
	glfwSetFramebufferSizeCallback(window, onFramebufferResized);
	glfwSetKeyCallback(window, onKey);
	glfwSetMouseButtonCallback(window, onMouseButton);
	glfwSetCursorPosCallback(window, onMouseMove);
	glfwSetScrollCallback(window, onMouseWheel);
	glfwSetDropCallback(window, onFileDragNDrop);

	glfwGetCursorPos(window, &mouseX, &mouseY);
	glfwGetFramebufferSize(window, &windowWidth, &windowHeight);
	mouseY = (double)windowHeight - mouseY;

	GLuint vertShader = compileShader(GL_VERTEX_SHADER, vertShaderSource);
	GLuint fragShader = compileShader(GL_FRAGMENT_SHADER, renderShaderSource);
	GLuint updateShader = compileShader(GL_FRAGMENT_SHADER, updateShaderSource);

	GLuint renderShaders[2];
	renderShaders[0] = vertShader;
	renderShaders[1] = fragShader;
	renderProgram = linkShaderProgram(renderShaders, 2);
	GLuint updateShaders[2];
	updateShaders[0] = vertShader;
	updateShaders[1] = updateShader;
	updateProgram = linkShaderProgram(updateShaders, 2);

	uniformScale = glGetUniformLocation(renderProgram, "scale");
	uniformOffset = glGetUniformLocation(renderProgram, "offset");
	uniformBorderSize = glGetUniformLocation(renderProgram, "borderSize");
	uniformBackgroundColor = glGetUniformLocation(renderProgram, "backgroundColor");
	uniformDeadColor = glGetUniformLocation(renderProgram, "deadColor");
	uniformAliveColor = glGetUniformLocation(renderProgram, "aliveColor");

	glDeleteShader(vertShader);
	glDeleteShader(fragShader);
	glDeleteShader(updateShader);

	const float quadData[4][2] = {
		{ -1, +1 },
		{ -1, -1 },
		{ +1, +1 },
		{ +1, -1 },
	};

	GLuint vertexArray;
	glGenVertexArrays(1, &vertexArray);
	glBindVertexArray(vertexArray);

	GLuint vertexBuffer;
	glGenBuffers(1, &vertexBuffer);
	glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadData), &quadData[0][0], GL_STATIC_DRAW);

	GLint updatePositionLocation = glGetAttribLocation(updateProgram, "position");
	GLint renderPositionLocation = glGetAttribLocation(renderProgram, "position");
	glEnableVertexAttribArray(updatePositionLocation);
	glVertexAttribPointer(updatePositionLocation, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(renderPositionLocation);
	glVertexAttribPointer(renderPositionLocation, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	numCellsX = ceilMultipleOf32(256);
	numCellsY = ceilMultipleOf32(256);
	cellsRead  = createTexture(NULL, numCellsX, numCellsY / 32, GL_RED_INTEGER, GL_R32UI);
	cellsWrite = createTexture(NULL, numCellsX, numCellsY / 32, GL_RED_INTEGER, GL_R32UI);
	cellsReadFramebuffer = createFramebuffer(cellsRead);
	cellsWriteFramebuffer = createFramebuffer(cellsWrite);
	glCheckErrors();

	clearCells();
	centerCellsOnScreen();

#ifdef BENCHMARK
	const char *benchFile = "digital-clock.rle";
	onFileDragNDrop(window, 1, &benchFile);
	int benchCellsX = numCellsX;
	int benchCellsY = numCellsY;
	vsyncIsOn = 0;
	glfwSwapInterval(0);
	glfwSetWindowTitle(window, "GPU Life - Benchmark");
	glfwSetWindowSize(window, 1920, 1080);
	centerCellsOnScreen();
	int numBenchmarkUpdates = 10240;
	printf("running benchmark ... ");
	glFinish();
	uint64_t startTime = glfwGetTimerValue();
	for (int i = 0; i < numBenchmarkUpdates; ++i)
		updateCells();
	renderCells();
	glfwSwapBuffers(window);
	glFinish();
	uint64_t endTime = glfwGetTimerValue();
	double benchTime = (endTime > startTime ? endTime - startTime : startTime - endTime) / (double)glfwGetTimerFrequency();
	double referenceTime = 3.20;
	printf("done\n");
	printf("total   %.2lf sec\n", benchTime);
	printf("average %.2lf ms per frame\n", benchTime * 1.0e+3 / numBenchmarkUpdates);
	printf("average %.2lf ps per cell\n", benchTime * 1.0e+12 / ((int64_t)numBenchmarkUpdates * (int64_t)benchCellsX * (int64_t)benchCellsY));
	printf("speedup x%.2lf\n", referenceTime / benchTime);
#endif

	uint64_t frameAccumulator1 = 0;
	uint64_t frameAccumulator2 = 0;
	double timeAccumulator = 0.0;
	uint64_t t0 = glfwGetTimerValue();
	double timerFrequency = (double)glfwGetTimerFrequency();
	double timerPeriod = 1.0 / timerFrequency;

	while (!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		uint64_t t1 = glfwGetTimerValue();
		double deltaTime = ((t1 > t0) ? t1 - t0 : t0 - t1) * timerPeriod;
		t0 = t1;

		timeAccumulator += deltaTime;
		frameAccumulator1 += 1;
		frameAccumulator2 += 1;

		if (isRunning && frameAccumulator1 >= framesPerUpdate) {
			frameAccumulator1 = 0;
			for (int i = 0; i < updatesPerFrame; ++i)
				updateCells();
		}
		renderCells();

		if (timeAccumulator > 0.05) {
			char title[512];
			double generationsPerFrame = updatesPerFrame > 1 ? updatesPerFrame : 1.0 / framesPerUpdate;
			if (isRunning)
				snprintf(title, sizeof(title), "GPU Life - %s - %lg steps per frame @ %.1lf fps - generation %d", 
					patternName, generationsPerFrame, frameAccumulator2 / timeAccumulator, generation);
			else
				snprintf(title, sizeof(title), "GPU Life - %s - %lg steps per frame @ PAUSED - generation %d", 
					patternName, generationsPerFrame, generation);
			glfwSetWindowTitle(window, title);
			timeAccumulator = 0;
			frameAccumulator2 = 0;
		}

		glfwSwapBuffers(window);
	}

	glCheckErrors();
	glDeleteTextures(1, &cellsRead);
	glDeleteTextures(1, &cellsWrite);
	glDeleteFramebuffers(1, &cellsReadFramebuffer);
	glDeleteFramebuffers(1, &cellsWriteFramebuffer);
	glDeleteProgram(renderProgram);
 	glDeleteProgram(updateProgram);
	glDeleteVertexArrays(1, &vertexArray);
	glDeleteBuffers(1, &vertexBuffer);
	glCheckErrors();
	free(patternName);
	glfwDestroyWindow(window);
	glfwTerminate();
	return 0;
}