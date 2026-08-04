#pragma once

#include "vm.h"
#include "runner.h"


typedef void (*NativeCodeFunc)(VMContext *ctx, Runner *runner, Instance *instance);


void NativeScripts_init(VMContext *ctx, Runner *runner);


void NativeScripts_reset(void);


NativeCodeFunc NativeScripts_find(const char *codeName);


void NativeScripts_register(const char *codeName, NativeCodeFunc func);
