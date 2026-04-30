//
// Created by Notebook on 30.04.2026.
//

#ifndef BUTTERSCOTCH_ICON_PARSE_H
#define BUTTERSCOTCH_ICON_PARSE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <NovaGL.h>

#include "stb_image.h"

#pragma pack(push,1)

typedef struct {
    uint16_t e_magic;
    uint8_t  _pad[58];
    uint32_t e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct {
    uint32_t Signature;
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint8_t  _pad[12];
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} IMAGE_FILE_HEADER;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} IMAGE_DATA_DIRECTORY;

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16

typedef struct {
    uint16_t Magic;
    uint8_t  _pad[94];
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32;

typedef struct {
    uint32_t Signature;
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS32;

typedef struct {
    uint8_t  Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint8_t  _pad[16];
} IMAGE_SECTION_HEADER;

typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
} IMAGE_RESOURCE_DIRECTORY;

typedef struct {
    uint32_t Name;
    uint32_t OffsetToData;
} IMAGE_RESOURCE_DIRECTORY_ENTRY;

typedef struct {
    uint32_t OffsetToData;
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
} IMAGE_RESOURCE_DATA_ENTRY;

typedef struct {
    uint8_t width, height, colorCount, reserved;
    uint16_t planes, bitCount;
    uint32_t bytesInRes;
    uint16_t id;
} GRPICONDIRENTRY;

#pragma pack(pop)

static GLuint extract_icon_from_exe_pe(const char *path);
static GLuint load_texture_from_file(const char *path);
uint16_t rd16(const uint8_t *p);
uint32_t rd32(const uint8_t *p);
static GLuint upload_rgba_texture(const uint8_t *pixels, int w, int h);
#endif //BUTTERSCOTCH_ICON_PARSE_H