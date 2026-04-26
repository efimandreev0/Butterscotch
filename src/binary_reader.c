#include "binary_reader.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

BinaryReader BinaryReader_create(FILE* file, size_t fileSize) {
    return (BinaryReader){
        .file = file,
        .fileSize = fileSize,
        .buffer = nullptr,
        .bufferBase = 0,
        .bufferSize = 0,
        .bufferPos = 0,
        .useFileFallback = false
    };
}

void BinaryReader_setBuffer(BinaryReader* reader, uint8_t* buffer, size_t baseOffset, size_t size) {
    reader->buffer = buffer;
    reader->bufferBase = baseOffset;
    reader->bufferSize = size;
    reader->bufferPos = 0;
    reader->useFileFallback = false;
}

void BinaryReader_clearBuffer(BinaryReader* reader) {
    reader->buffer = nullptr;
    reader->bufferBase = 0;
    reader->bufferSize = 0;
    reader->bufferPos = 0;
    reader->useFileFallback = false;
}

static void readCheck(BinaryReader* reader, void* dest, size_t bytes) {
    // If we are operating inside the RAM buffer...
    if (reader->buffer != nullptr && !reader->useFileFallback) {
        // If we cross the boundary, automatically fallback to file reading
        if (reader->bufferPos + bytes > reader->bufferSize) {
            // Если файла нет (режим "только ОЗУ"), фоллбэк невозможен
            if (reader->file == NULL) {
                size_t absPos = reader->bufferBase + reader->bufferPos;
                fprintf(stderr, "BinaryReader: pure buffer read error at position 0x%zX (requested %zu, remaining %zu)\n", absPos, bytes, reader->bufferSize - reader->bufferPos);
                exit(1);
            }
            size_t absPos = reader->bufferBase + reader->bufferPos;
            reader->useFileFallback = true;
            fseek(reader->file, (long)absPos, SEEK_SET);
        } else {
            // Fast path: memory copy
            memcpy(dest, reader->buffer + reader->bufferPos, bytes);
            reader->bufferPos += bytes;
            return;
        }
    }

    if (reader->file == NULL) {
        fprintf(stderr, "BinaryReader: attempted file read but file is NULL\n");
        exit(1);
    }

    // Fallback or Normal File read
    size_t read = fread(dest, 1, bytes, reader->file);
    if (read != bytes) {
        long pos = ftell(reader->file) - (long) read;
        fprintf(stderr, "BinaryReader: read error at position 0x%lX (requested %zu bytes, got %zu, file size 0x%zX)\n", pos, bytes, read, reader->fileSize);
        exit(1);
    }
}

uint8_t BinaryReader_readUint8(BinaryReader* reader) {
    uint8_t value;
    readCheck(reader, &value, 1);
    return value;
}

int16_t BinaryReader_readInt16(BinaryReader* reader) {
    int16_t value;
    readCheck(reader, &value, 2);
    return value;
}

uint16_t BinaryReader_readUint16(BinaryReader* reader) {
    uint16_t value;
    readCheck(reader, &value, 2);
    return value;
}

int32_t BinaryReader_readInt32(BinaryReader* reader) {
    int32_t value;
    readCheck(reader, &value, 4);
    return value;
}

uint32_t BinaryReader_readUint32(BinaryReader* reader) {
    uint32_t value;
    readCheck(reader, &value, 4);
    return value;
}

float BinaryReader_readFloat32(BinaryReader* reader) {
    float value;
    readCheck(reader, &value, 4);
    return value;
}

uint64_t BinaryReader_readUint64(BinaryReader* reader) {
    uint64_t value;
    readCheck(reader, &value, 8);
    return value;
}

int64_t BinaryReader_readInt64(BinaryReader* reader) {
    int64_t value;
    readCheck(reader, &value, 8);
    return value;
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
        if (reader->file == NULL) {
            fprintf(stderr, "BinaryReader: pure buffer readBytesAt 0x%zX out of bounds\n", offset);
            exit(1);
        }
    }

    if (reader->file == NULL) {
        fprintf(stderr, "BinaryReader: readBytesAt attempted file read but file is NULL\n");
        exit(1);
    }

    long savedPos = ftell(reader->file);
    fseek(reader->file, (long) offset, SEEK_SET);
    size_t read = fread(buf, 1, count, reader->file);
    if (read != count) {
        fprintf(stderr, "BinaryReader: readBytesAt read error at 0x%zX\n", offset);
        exit(1);
    }
    fseek(reader->file, savedPos, SEEK_SET);

    return buf;
}

void BinaryReader_skip(BinaryReader* reader, size_t bytes) {
    if (reader->buffer != nullptr && !reader->useFileFallback) {
        if (reader->bufferPos + bytes <= reader->bufferSize) {
            reader->bufferPos += bytes;
            return;
        } else {
            if (reader->file == NULL) {
                fprintf(stderr, "BinaryReader: pure buffer skip out of bounds\n");
                exit(1);
            }
            size_t absPos = reader->bufferBase + reader->bufferPos;
            reader->useFileFallback = true;
            fseek(reader->file, (long)(absPos + bytes), SEEK_SET);
            return;
        }
    }

    if (reader->file != NULL) {
        fseek(reader->file, (long) bytes, SEEK_CUR);
    }
}

void BinaryReader_seek(BinaryReader* reader, size_t position) {
    // Делаем проверку размера файла ТОЛЬКО если файл существует и мы знаем его размер
    if (reader->file != NULL && reader->fileSize > 0 && position > reader->fileSize) {
        fprintf(stderr, "BinaryReader: seek to 0x%zX out of bounds (file size 0x%zX)\n", position, reader->fileSize);
        exit(1);
    }

    if (reader->buffer != nullptr) {
        // Проверяем, находится ли смещение внутри нашего буфера
        if (position >= reader->bufferBase && position <= reader->bufferBase + reader->bufferSize) {
            reader->bufferPos = position - reader->bufferBase;
            reader->useFileFallback = false; // Возвращаемся к чтению из RAM
            return;
        } else {
            if (reader->file == NULL) {
                // Если мы читаем комнату в памяти (file == NULL), за её пределы выходить нельзя
                fprintf(stderr, "BinaryReader: buffer seek to 0x%zX out of RAM range [0x%zX, 0x%zX]\n", position, reader->bufferBase, reader->bufferBase + reader->bufferSize);
                exit(1);
            }
            // Вышли за пределы: включаем фоллбэк и прыгаем лазером по файлу
            reader->useFileFallback = true;
            fseek(reader->file, (long) position, SEEK_SET);
            return;
        }
    }

    if (reader->file != NULL) {
        fseek(reader->file, (long) position, SEEK_SET);
    }
}

size_t BinaryReader_getPosition(BinaryReader* reader) {
    if (reader->buffer != nullptr && !reader->useFileFallback) {
        return reader->bufferBase + reader->bufferPos;
    }
    if (reader->file != NULL) {
        return (size_t) ftell(reader->file);
    }
    return 0;
}