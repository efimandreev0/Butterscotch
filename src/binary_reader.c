#include "binary_reader.h"
#include "binary_utils.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

BinaryReader BinaryReader_create(FILE* file, size_t fileSize) {
    return (BinaryReader){.file = file, .fileSize = fileSize, .buffer = nullptr, .bufferBase = 0, .bufferSize = 0, .bufferPos = 0, .bufferContext = nullptr};
}

void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size) {
    reader->buffer = buffer;
    reader->bufferBase = baseOffset;
    reader->bufferSize = size;
    reader->bufferPos = 0;
    reader->bufferContext = nullptr;
}

void BinaryReader_setBufferContext(BinaryReader* reader, const char* context) {
    reader->bufferContext = context;
}

void BinaryReader_clearBuffer(BinaryReader* reader) {
    reader->buffer = nullptr;
    reader->bufferBase = 0;
    reader->bufferSize = 0;
    reader->bufferPos = 0;
    reader->bufferContext = nullptr;
}

static void readCheck(BinaryReader* reader, void* dest, size_t bytes) {
    if (reader->buffer != nullptr) {
        // Если чтение полностью умещается в буфер чанка
        if (reader->bufferPos <= reader->bufferSize && (reader->bufferPos + bytes) <= reader->bufferSize) {
            memcpy(dest, reader->buffer + reader->bufferPos, bytes);
            reader->bufferPos += bytes;
            return;
        } else {
            // Вышли за пределы буфера (привет модам) - фоллбэк на чтение из файла!
            size_t absPos = reader->bufferBase + reader->bufferPos;
            fseek(reader->file, (long)absPos, SEEK_SET);

            size_t read = fread(dest, 1, bytes, reader->file);
            if (read != bytes) {
                fprintf(stderr, "BinaryReader: fallback read error at 0x%zX (req %zu, got %zu)\n", absPos, bytes, read);
                abort();
            }
            reader->bufferPos += bytes; // Поддерживаем виртуальную позицию
            return;
        }
    }

    size_t read = fread(dest, 1, bytes, reader->file);
    if (read != bytes) {
        long pos = ftell(reader->file) - (long) read;
        fprintf(stderr, "BinaryReader: read error at 0x%lX (req %zu, got %zu)\n", pos, bytes, read);
        abort();
    }
}

uint8_t BinaryReader_readUint8(BinaryReader* reader) {
    uint8_t value;
    readCheck(reader, &value, 1);
    return value;
}

int16_t BinaryReader_readInt16(BinaryReader* reader) {
    uint16_t value;
    readCheck(reader, &value, sizeof(value));
    return (int16_t) BinaryUtils_toLittle16(value);
}

uint16_t BinaryReader_readUint16(BinaryReader* reader) {
    uint16_t value;
    readCheck(reader, &value, sizeof(value));
    return BinaryUtils_toLittle16(value);
}

int32_t BinaryReader_readInt32(BinaryReader* reader) {
    uint32_t value;
    readCheck(reader, &value, sizeof(value));
    return (int32_t) BinaryUtils_toLittle32(value);
}

uint32_t BinaryReader_readUint32(BinaryReader* reader) {
    uint32_t value;
    readCheck(reader, &value, sizeof(value));
    return BinaryUtils_toLittle32(value);
}

float BinaryReader_readFloat32(BinaryReader* reader) {
    uint32_t bits;
    float value;
    readCheck(reader, &bits, sizeof(bits));
    bits = BinaryUtils_toLittle32(bits);
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t BinaryReader_readUint64(BinaryReader* reader) {
    uint64_t value;
    readCheck(reader, &value, sizeof(value));
    return BinaryUtils_toLittle64(value);
}

int64_t BinaryReader_readInt64(BinaryReader* reader) {
    uint64_t value;
    readCheck(reader, &value, sizeof(value));
    return (int64_t) BinaryUtils_toLittle64(value);
}

bool BinaryReader_readBool32(BinaryReader* reader) {
    return BinaryReader_readUint32(reader) != 0;
}

void BinaryReader_readBytes(BinaryReader* reader, void* dest, size_t count) {
    readCheck(reader, dest, count);
}

uint8_t* BinaryReader_readBytesAt(BinaryReader* reader, size_t offset, size_t count) {
    uint8_t* buf = safeMalloc(count);

    if (reader->buffer != nullptr) {
        if (offset >= reader->bufferBase && (offset + count) <= (reader->bufferBase + reader->bufferSize)) {
            memcpy(buf, reader->buffer + (offset - reader->bufferBase), count);
            return buf;
        }
    }

    long savedPos = ftell(reader->file);
    fseek(reader->file, (long) offset, SEEK_SET);
    size_t read = fread(buf, 1, count, reader->file);
    if (read != count) {
        fprintf(stderr, "BinaryReader: readBytesAt error at 0x%zX\n", offset);
        abort();
    }
    fseek(reader->file, savedPos, SEEK_SET);
    return buf;
}

void BinaryReader_skip(BinaryReader* reader, size_t bytes) {
    if (reader->buffer != nullptr) {
        reader->bufferPos += bytes;
        return;
    }
    fseek(reader->file, (long) bytes, SEEK_CUR);
}

void BinaryReader_seek(BinaryReader* reader, size_t position) {
    if (reader->buffer != nullptr) {
        // Вычисляем виртуальную позицию. Если вылетит за пределы - readCheck аккуратно всё разрулит!
        reader->bufferPos = position - reader->bufferBase;
        return;
    }

    if (position > reader->fileSize) {
        fprintf(stderr, "BinaryReader: seek to 0x%zX out of bounds\n", position);
        abort();
    }
    fseek(reader->file, (long) position, SEEK_SET);
}

size_t BinaryReader_getPosition(BinaryReader* reader) {
    if (reader->buffer != nullptr) {
        return reader->bufferBase + reader->bufferPos;
    }
    return (size_t) ftell(reader->file);
}
