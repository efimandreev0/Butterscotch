#include "gl_renderer.h"
#include "../../matrix_math.h"
#include "../../utils.h"

#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "stb_image.h"

// ===[ Constants ]===
#define MAX_QUADS 4096
#define FLOATS_PER_VERTEX 8  // x, y, u, v, r, g, b, a
#define VERTICES_PER_QUAD 4
#define INDICES_PER_QUAD 6

// ===[ Shader Sources ]===
static const char* vertexShaderSource =
    "#version 410 core\n"
    "layout(location = 0) in vec2 aPos;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "uniform mat4 uProjection;\n"
    "out vec2 vTexCoord;\n"
    "out vec4 vColor;\n"
    "void main() {\n"
    "    gl_Position = uProjection * vec4(aPos, 0.0, 1.0);\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColor = aColor;\n"
    "}\n";

static const char* fragmentShaderSource =
    "#version 410 core\n"
    "in vec2 vTexCoord;\n"
    "in vec4 vColor;\n"
    "uniform sampler2D uTexture;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(uTexture, vTexCoord) * vColor;\n"
    "}\n";

// ===[ Shader Compilation ]===

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        fprintf(stderr, "GL: Shader compilation failed: %s\n", infoLog);
        abort();
    }
    return shader;
}

static GLuint linkProgram(GLuint vertShader, GLuint fragShader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        fprintf(stderr, "GL: Shader linking failed: %s\n", infoLog);
        abort();
    }
    return program;
}

// ===[ Batch Flush ]===

static void flushBatch(GLRenderer* gl) {
    if (gl->quadCount == 0) return;

    int32_t vertexCount = gl->quadCount * VERTICES_PER_QUAD;
    int32_t indexCount = gl->quadCount * INDICES_PER_QUAD;

    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * FLOATS_PER_VERTEX * sizeof(float), gl->vertexData);

    glBindTexture(GL_TEXTURE_2D, gl->currentTextureId);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);

    gl->quadCount = 0;
}

// ===[ Vtable Implementations ]===

static void glInit(Renderer* renderer, DataWin* dataWin) {
    GLRenderer* gl = (GLRenderer*) renderer;
    renderer->dataWin = dataWin;

    // Compile shaders
    GLuint vertShader = compileShader(GL_VERTEX_SHADER, vertexShaderSource);
    GLuint fragShader = compileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);
    gl->shaderProgram = linkProgram(vertShader, fragShader);
    glDeleteShader(vertShader);
    glDeleteShader(fragShader);

    gl->uProjection = glGetUniformLocation(gl->shaderProgram, "uProjection");
    gl->uTexture = glGetUniformLocation(gl->shaderProgram, "uTexture");

    // Create VAO/VBO/EBO
    glGenVertexArrays(1, &gl->vao);
    glGenBuffers(1, &gl->vbo);
    glGenBuffers(1, &gl->ebo);

    glBindVertexArray(gl->vao);

    // VBO: sized for max quads
    int32_t vboSize = MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX * (int32_t) sizeof(float);
    glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
    glBufferData(GL_ARRAY_BUFFER, vboSize, nullptr, GL_DYNAMIC_DRAW);

    // EBO: pre-fill with quad index pattern (0,1,2,2,3,0 repeated)
    int32_t eboSize = MAX_QUADS * INDICES_PER_QUAD * (int32_t) sizeof(uint32_t);
    uint32_t* indices = malloc(eboSize);
    for (int32_t i = 0; MAX_QUADS > i; i++) {
        uint32_t base = (uint32_t) i * 4;
        indices[i * 6 + 0] = base + 0;
        indices[i * 6 + 1] = base + 1;
        indices[i * 6 + 2] = base + 2;
        indices[i * 6 + 3] = base + 2;
        indices[i * 6 + 4] = base + 3;
        indices[i * 6 + 5] = base + 0;
    }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl->ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, eboSize, indices, GL_STATIC_DRAW);
    free(indices);

    // Vertex attributes: pos(2f), texcoord(2f), color(4f)
    int32_t stride = FLOATS_PER_VERTEX * (int32_t) sizeof(float);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*) 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*) (2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (void*) (4 * sizeof(float)));
    glEnableVertexAttribArray(2);

    glBindVertexArray(0);

    // Allocate CPU-side vertex buffer
    gl->vertexData = malloc(MAX_QUADS * VERTICES_PER_QUAD * FLOATS_PER_VERTEX * sizeof(float));

    // Load textures from TXTR pages
    gl->textureCount = dataWin->txtr.count;
    gl->glTextures = malloc(gl->textureCount * sizeof(GLuint));
    gl->textureWidths = malloc(gl->textureCount * sizeof(int32_t));
    gl->textureHeights = malloc(gl->textureCount * sizeof(int32_t));

    glGenTextures((GLsizei) gl->textureCount, gl->glTextures);

    for (uint32_t i = 0; gl->textureCount > i; i++) {
        Texture* txtr = &dataWin->txtr.textures[i];
        uint8_t* pngData = dataWin->fileBuffer + txtr->blobOffset;
        uint32_t pngSize = txtr->blobSize;

        int w, h, channels;
        uint8_t* pixels = stbi_load_from_memory(pngData, (int) pngSize, &w, &h, &channels, 4);
        if (pixels == nullptr) {
            fprintf(stderr, "GL: Failed to decode TXTR page %u\n", i);
            gl->textureWidths[i] = 0;
            gl->textureHeights[i] = 0;
            continue;
        }

        gl->textureWidths[i] = w;
        gl->textureHeights[i] = h;

        glBindTexture(GL_TEXTURE_2D, gl->glTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        stbi_image_free(pixels);
        fprintf(stderr, "GL: Loaded TXTR page %u (%dx%d)\n", i, w, h);
    }

    // Create 1x1 white pixel texture for primitive drawing (rectangles, lines, etc.)
    glGenTextures(1, &gl->whiteTexture);
    glBindTexture(GL_TEXTURE_2D, gl->whiteTexture);
    uint8_t whitePixel[4] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Enable blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    gl->quadCount = 0;
    gl->currentTextureId = 0;

    // Create FBO (texture will be allocated/resized in beginFrame)
    glGenFramebuffers(1, &gl->fbo);
    gl->fboTexture = 0;
    gl->fboWidth = 0;
    gl->fboHeight = 0;

    fprintf(stderr, "GL: Renderer initialized (%u texture pages)\n", gl->textureCount);
}

static void glDestroy(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;

    if (gl->fboTexture != 0) glDeleteTextures(1, &gl->fboTexture);
    glDeleteFramebuffers(1, &gl->fbo);
    glDeleteTextures(1, &gl->whiteTexture);

    glDeleteTextures((GLsizei) gl->textureCount, gl->glTextures);
    glDeleteProgram(gl->shaderProgram);
    glDeleteVertexArrays(1, &gl->vao);
    glDeleteBuffers(1, &gl->vbo);
    glDeleteBuffers(1, &gl->ebo);

    free(gl->glTextures);
    free(gl->textureWidths);
    free(gl->textureHeights);
    free(gl->vertexData);
    free(gl);
}

static void glBeginFrame(Renderer* renderer, int32_t viewX, int32_t viewY, int32_t viewW, int32_t viewH, int32_t windowW, int32_t windowH) {
    GLRenderer* gl = (GLRenderer*) renderer;

    gl->quadCount = 0;
    gl->currentTextureId = 0;

    // Store window size for endFrame blit
    gl->windowW = windowW;
    gl->windowH = windowH;

    // Resize FBO texture if view dimensions changed
    if (viewW != gl->fboWidth || viewH != gl->fboHeight) {
        if (gl->fboTexture != 0) glDeleteTextures(1, &gl->fboTexture);

        glGenTextures(1, &gl->fboTexture);
        glBindTexture(GL_TEXTURE_2D, gl->fboTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, viewW, viewH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl->fboTexture, 0);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "GL: Framebuffer incomplete (status=0x%X)\n", status);
        }

        gl->fboWidth = viewW;
        gl->fboHeight = viewH;
        fprintf(stderr, "GL: FBO resized to %dx%d\n", viewW, viewH);
    }

    // Bind FBO -- all rendering goes to the FBO texture
    glBindFramebuffer(GL_FRAMEBUFFER, gl->fbo);
    glViewport(0, 0, viewW, viewH);

    // Build orthographic projection (Y-down for GML coordinate system)
    Matrix4f projection;
    Matrix4f_identity(&projection);
    Matrix4f_ortho(&projection, (float) viewX, (float) (viewX + viewW), (float) (viewY + viewH), (float) viewY, -1.0f, 1.0f);

    glUseProgram(gl->shaderProgram);
    glUniformMatrix4fv(gl->uProjection, 1, GL_FALSE, projection.m);
    glUniform1i(gl->uTexture, 0);
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(gl->vao);
}

static void glEndFrame(Renderer* renderer) {
    GLRenderer* gl = (GLRenderer*) renderer;
    flushBatch(gl);
    glBindVertexArray(0);

    // Blit FBO to default framebuffer (screen)
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, gl->fboWidth, gl->fboHeight, 0, 0, gl->windowW, gl->windowH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void glRendererFlush(Renderer* renderer) {
    flushBatch((GLRenderer*) renderer);
}

static void glDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    GLRenderer* gl = (GLRenderer*) renderer;
    DataWin* dw = renderer->dataWin;

    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;

    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int16_t pageId = tpag->texturePageId;
    if (0 > pageId || gl->textureCount <= (uint32_t) pageId) return;

    GLuint texId = gl->glTextures[pageId];
    int32_t texW = gl->textureWidths[pageId];
    int32_t texH = gl->textureHeights[pageId];
    if (texW == 0 || texH == 0) return;

    // Flush if texture changed or batch full
    if (gl->quadCount > 0 && gl->currentTextureId != texId) {
        flushBatch(gl);
    }
    if (gl->quadCount >= MAX_QUADS) {
        flushBatch(gl);
    }
    gl->currentTextureId = texId;

    // Compute normalized UVs from TPAG source rect
    float u0 = (float) tpag->sourceX / (float) texW;
    float v0 = (float) tpag->sourceY / (float) texH;
    float u1 = (float) (tpag->sourceX + tpag->sourceWidth) / (float) texW;
    float v1 = (float) (tpag->sourceY + tpag->sourceHeight) / (float) texH;

    // Compute local quad corners (relative to origin, with target offset)
    float localX0 = (float) tpag->targetX - originX;
    float localY0 = (float) tpag->targetY - originY;
    float localX1 = localX0 + (float) tpag->sourceWidth;
    float localY1 = localY0 + (float) tpag->sourceHeight;

    // Build 2D transform: T(x,y) * R(-angleDeg) * S(xscale, yscale)
    // GML rotation is counter-clockwise, OpenGL rotation is counter-clockwise, but
    // since we have Y-down, we negate the angle to get the correct visual rotation
    float angleRad = -angleDeg * ((float) M_PI / 180.0f);
    Matrix4f transform;
    Matrix4f_setTransform2D(&transform, x, y, xscale, yscale, angleRad);

    // Transform 4 corners
    float x0, y0, x1, y1, x2, y2, x3, y3;
    Matrix4f_transformPoint(&transform, localX0, localY0, &x0, &y0); // top-left
    Matrix4f_transformPoint(&transform, localX1, localY0, &x1, &y1); // top-right
    Matrix4f_transformPoint(&transform, localX1, localY1, &x2, &y2); // bottom-right
    Matrix4f_transformPoint(&transform, localX0, localY1, &x3, &y3); // bottom-left

    // Convert BGR color to RGB floats
    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    // Write 4 vertices into batch buffer
    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // Vertex 0: top-left
    verts[0] = x0; verts[1] = y0; verts[2] = u0; verts[3] = v0;
    verts[4] = r;  verts[5] = g;  verts[6] = b;  verts[7] = alpha;

    // Vertex 1: top-right
    verts[8]  = x1; verts[9]  = y1; verts[10] = u1; verts[11] = v0;
    verts[12] = r;  verts[13] = g;  verts[14] = b;  verts[15] = alpha;

    // Vertex 2: bottom-right
    verts[16] = x2; verts[17] = y2; verts[18] = u1; verts[19] = v1;
    verts[20] = r;  verts[21] = g;  verts[22] = b;  verts[23] = alpha;

    // Vertex 3: bottom-left
    verts[24] = x3; verts[25] = y3; verts[26] = u0; verts[27] = v1;
    verts[28] = r;  verts[29] = g;  verts[30] = b;  verts[31] = alpha;

    gl->quadCount++;
}

// Emits a single colored quad into the batch using the white pixel texture
static void emitColoredQuad(GLRenderer* gl, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    if (gl->quadCount > 0 && gl->currentTextureId != gl->whiteTexture) {
        flushBatch(gl);
    }
    if (gl->quadCount >= MAX_QUADS) {
        flushBatch(gl);
    }
    gl->currentTextureId = gl->whiteTexture;

    float* verts = gl->vertexData + gl->quadCount * VERTICES_PER_QUAD * FLOATS_PER_VERTEX;

    // All UVs point to (0.5, 0.5) center of the 1x1 white texture
    // Vertex 0: top-left
    verts[0] = x0; verts[1] = y0; verts[2] = 0.5f; verts[3] = 0.5f;
    verts[4] = r;  verts[5] = g;  verts[6] = b;    verts[7] = a;

    // Vertex 1: top-right
    verts[8]  = x1; verts[9]  = y0; verts[10] = 0.5f; verts[11] = 0.5f;
    verts[12] = r;  verts[13] = g;  verts[14] = b;    verts[15] = a;

    // Vertex 2: bottom-right
    verts[16] = x1; verts[17] = y1; verts[18] = 0.5f; verts[19] = 0.5f;
    verts[20] = r;  verts[21] = g;  verts[22] = b;    verts[23] = a;

    // Vertex 3: bottom-left
    verts[24] = x0; verts[25] = y1; verts[26] = 0.5f; verts[27] = 0.5f;
    verts[28] = r;  verts[29] = g;  verts[30] = b;    verts[31] = a;

    gl->quadCount++;
}

static void glDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    GLRenderer* gl = (GLRenderer*) renderer;

    float r = (float) BGR_R(color) / 255.0f;
    float g = (float) BGR_G(color) / 255.0f;
    float b = (float) BGR_B(color) / 255.0f;

    if (outline) {
        // Draw 4 one-pixel-wide edges: top, bottom, left, right
        emitColoredQuad(gl, x1, y1, x2 + 1, y1 + 1, r, g, b, alpha); // top
        emitColoredQuad(gl, x1, y2, x2 + 1, y2 + 1, r, g, b, alpha); // bottom
        emitColoredQuad(gl, x1, y1 + 1, x1 + 1, y2, r, g, b, alpha); // left
        emitColoredQuad(gl, x2, y1 + 1, x2 + 1, y2, r, g, b, alpha); // right
    } else {
        // Filled rectangle: GML adds +1 to width/height for filled rects
        emitColoredQuad(gl, x1, y1, x2 + 1, y2 + 1, r, g, b, alpha);
    }
}

// ===[ Vtable ]===

static RendererVtable glVtable = {
    .init = glInit,
    .destroy = glDestroy,
    .beginFrame = glBeginFrame,
    .endFrame = glEndFrame,
    .drawSprite = glDrawSprite,
    .drawRectangle = glDrawRectangle,
    .flush = glRendererFlush,
};

// ===[ Public API ]===

Renderer* GLRenderer_create(void) {
    GLRenderer* gl = calloc(1, sizeof(GLRenderer));
    gl->base.vtable = &glVtable;
    gl->base.drawColor = 0xFFFFFF; // white (BGR)
    gl->base.drawAlpha = 1.0f;
    gl->base.drawFont = -1;
    gl->base.drawHalign = 0;
    gl->base.drawValign = 0;
    return (Renderer*) gl;
}
