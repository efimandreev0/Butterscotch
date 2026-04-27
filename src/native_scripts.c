#include "native_scripts.h"
#include "data_win.h"
#include "instance.h"
#include "collision.h"
#include "renderer.h"
#include "rvalue.h"
#include "text_utils.h"
#include "utils.h"
#include "vm.h"
#include "vm_builtins.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PSP
#include <psprtc.h>
#endif

#include "stb_ds.h"


uint32_t g_writerTimeUs = 0;
int32_t g_writerCalls = 0;


static Instance* findInstanceByObject(Runner* runner, int32_t objIdx);



static struct { char* key; NativeCodeFunc value; }* nativeOverrideMap = nullptr;

static void registerNative(const char* codeName, NativeCodeFunc func) {
    shput(nativeOverrideMap, (char*) codeName, func);
}

void NativeScripts_register(const char* codeName, NativeCodeFunc func) {
    shput(nativeOverrideMap, (char*) codeName, func);
}

NativeCodeFunc NativeScripts_find(const char* codeName) {
    ptrdiff_t idx = shgeti(nativeOverrideMap, (char*) codeName);
    if (0 > idx) return nullptr;
    return nativeOverrideMap[idx].value;
}


static int32_t findSelfVarId(DataWin* dw, const char* name) {
    forEach(Variable, v, dw->vari.variables, dw->vari.variableCount) {
        if (v->varID >= 0 && v->instanceType != INSTANCE_GLOBAL && strcmp(v->name, name) == 0) {
            return v->varID;
        }
    }
    fprintf(stderr, "NativeScripts: WARNING - self variable '%s' not found in VARI chunk\n", name);
    return -1;
}


static int32_t findGlobalVarId(VMContext* ctx, const char* name) {
    ptrdiff_t idx = shgeti(ctx->globalVarNameMap, (char*) name);
    if (0 > idx) {
        fprintf(stderr, "NativeScripts: WARNING - global variable '%s' not found\n", name);
        return -1;
    }
    return ctx->globalVarNameMap[idx].value;
}


static int32_t findFontIndex(DataWin* dw, const char* name) {
    repeat(dw->font.count, i) {
        if (strcmp(dw->font.fonts[i].name, name) == 0) return (int32_t) i;
    }
    fprintf(stderr, "NativeScripts: WARNING - font '%s' not found\n", name);
    return -1;
}

static int32_t findObjectIndex(DataWin* dw, const char* name) {
    repeat(dw->objt.count, i) {
        if (strcmp(dw->objt.objects[i].name, name) == 0) return (int32_t) i;
    }
    fprintf(stderr, "NativeScripts: WARNING - object '%s' not found\n", name);
    return -1;
}


static int32_t findScriptCodeId(VMContext* ctx, const char* name) {
    ptrdiff_t idx = shgeti(ctx->funcMap, (char*) name);
    if (0 > idx) {
        fprintf(stderr, "NativeScripts: WARNING - script '%s' not found in funcMap\n", name);
        return -1;
    }
    return ctx->funcMap[idx].value;
}


static void setDirection(Instance* inst, GMLReal value) {
    GMLReal d = GMLReal_fmod(value, 360.0);
    if (0.0 > d) d += 360.0;
    inst->direction = d;
    Instance_computeComponentsFromSpeed(inst);
}


static GMLReal selfReal(Instance* inst, int32_t varId) {
    return RValue_toReal(Instance_getSelfVar(inst, varId));
}


static int32_t selfInt(Instance* inst, int32_t varId) {
    return RValue_toInt32(Instance_getSelfVar(inst, varId));
}


static const char* selfString(Instance* inst, int32_t varId) {
    RValue val = Instance_getSelfVar(inst, varId);
    if (val.type == RVALUE_STRING && val.string != nullptr) return val.string;
    return "";
}


static RValue selfArrayGet(Instance* inst, int32_t varId, int32_t index) {
    int64_t k = ((int64_t) varId << 32) | (uint32_t) index;
    ptrdiff_t idx = hmgeti(inst->selfArrayMap, k);
    if (0 > idx) return RValue_makeReal(0.0);
    RValue result = inst->selfArrayMap[idx].value;
    result.ownsString = false;
    return result;
}


static void selfArraySet(Instance* inst, int32_t varId, int32_t index, RValue val) {
    int64_t k = ((int64_t) varId << 32) | (uint32_t) index;
    ptrdiff_t idx = hmgeti(inst->selfArrayMap, k);
    if (idx >= 0) {
        RValue_free(&inst->selfArrayMap[idx].value);
        inst->selfArrayMap[idx].value = val;
    } else {
        hmput(inst->selfArrayMap, k, val);
    }
}


static GMLReal globalReal(VMContext* ctx, int32_t varId) {
    if (0 > varId || (uint32_t) varId >= ctx->globalVarCount) return 0.0;
    return RValue_toReal(ctx->globalVars[varId]);
}


static const char* globalString(VMContext* ctx, int32_t varId) {
    if (0 > varId || (uint32_t) varId >= ctx->globalVarCount) return "";
    RValue val = ctx->globalVars[varId];
    if (val.type == RVALUE_STRING && val.string != nullptr) return val.string;
    return "";
}


static void globalSet(VMContext* ctx, int32_t varId, RValue val) {
    if (0 > varId || (uint32_t) varId >= ctx->globalVarCount) return;
    RValue_free(&ctx->globalVars[varId]);
    if (val.type == RVALUE_STRING && val.string != nullptr) {
        ctx->globalVars[varId] = RValue_makeOwnedString(safeStrdup(val.string));
    } else {
        ctx->globalVars[varId] = val;
    }
}


static void globalArraySet(VMContext* ctx, int32_t varId, int32_t index, RValue val) {
    int64_t k = ((int64_t) varId << 32) | (uint32_t) index;
    ptrdiff_t idx = hmgeti(ctx->globalArrayMap, k);
    if (idx >= 0) {
        RValue_free(&ctx->globalArrayMap[idx].value);
    }
    if (val.type == RVALUE_STRING && !val.ownsString && val.string != nullptr) {
        val = RValue_makeOwnedString(safeStrdup(val.string));
    }
    hmput(ctx->globalArrayMap, k, val);
}


static RValue callBuiltin(VMContext* ctx, const char* name, RValue* args, int32_t argCount) {
    BuiltinFunc func = VMBuiltins_find(name);
    if (func == nullptr) {
        fprintf(stderr, "NativeScripts: builtin '%s' not found\n", name);
        return RValue_makeUndefined();
    }
    return func(ctx, args, argCount);
}


static struct {
    bool initialized;
    
    int32_t vtext, writingxend, vspacing, writingx, writingy;
    int32_t stringpos, originalstring, mycolor, myfont, shake;
    int32_t halt, dfy, stringno, mystring, textspeed, spacing;
    int32_t htextscale, vtextscale, myx, myy;
    
    int32_t gFlag, gFaceemotion, gFacechoice, gFacechange, gTyper, gLanguage;
    
    int32_t fntPapyrus, fntJaPapyrusBtl, fntJaMain, fntJaMaintext;
    int32_t fntMain, fntMaintext, fntComicsans, fntJaComicsans, fntJaComicsansBig;
    int32_t fntJaPapyrus;
    
    int32_t objPapdate;
    
    int32_t scrTexttype, scrNewline, scrReplaceButtonsPc, scrGetbuttonsprite, scrSetfont;
} writerCache;

static void initWriterCache(VMContext* ctx, DataWin* dw) {
    if (writerCache.initialized) return;
    writerCache.initialized = true;

    
    writerCache.vtext = findSelfVarId(dw, "vtext");
    writerCache.writingxend = findSelfVarId(dw, "writingxend");
    writerCache.vspacing = findSelfVarId(dw, "vspacing");
    writerCache.writingx = findSelfVarId(dw, "writingx");
    writerCache.writingy = findSelfVarId(dw, "writingy");
    writerCache.stringpos = findSelfVarId(dw, "stringpos");
    writerCache.originalstring = findSelfVarId(dw, "originalstring");
    writerCache.mycolor = findSelfVarId(dw, "mycolor");
    writerCache.myfont = findSelfVarId(dw, "myfont");
    writerCache.shake = findSelfVarId(dw, "shake");
    writerCache.halt = findSelfVarId(dw, "halt");
    writerCache.dfy = findSelfVarId(dw, "dfy");
    writerCache.stringno = findSelfVarId(dw, "stringno");
    writerCache.mystring = findSelfVarId(dw, "mystring");
    writerCache.textspeed = findSelfVarId(dw, "textspeed");
    writerCache.spacing = findSelfVarId(dw, "spacing");
    writerCache.htextscale = findSelfVarId(dw, "htextscale");
    writerCache.vtextscale = findSelfVarId(dw, "vtextscale");
    writerCache.myx = findSelfVarId(dw, "myx");
    writerCache.myy = findSelfVarId(dw, "myy");

    
    writerCache.gFlag = findGlobalVarId(ctx, "flag");
    writerCache.gFaceemotion = findGlobalVarId(ctx, "faceemotion");
    writerCache.gFacechoice = findGlobalVarId(ctx, "facechoice");
    writerCache.gFacechange = findGlobalVarId(ctx, "facechange");
    writerCache.gTyper = findGlobalVarId(ctx, "typer");
    writerCache.gLanguage = findGlobalVarId(ctx, "language");

    
    writerCache.fntPapyrus = findFontIndex(dw, "fnt_papyrus");
    writerCache.fntJaPapyrusBtl = findFontIndex(dw, "fnt_ja_papyrus_btl");
    writerCache.fntJaMain = findFontIndex(dw, "fnt_ja_main");
    writerCache.fntJaMaintext = findFontIndex(dw, "fnt_ja_maintext");
    writerCache.fntMain = findFontIndex(dw, "fnt_main");
    writerCache.fntMaintext = findFontIndex(dw, "fnt_maintext");
    writerCache.fntComicsans = findFontIndex(dw, "fnt_comicsans");
    writerCache.fntJaComicsans = findFontIndex(dw, "fnt_ja_comicsans");
    writerCache.fntJaComicsansBig = findFontIndex(dw, "fnt_ja_comicsans_big");
    writerCache.fntJaPapyrus = findFontIndex(dw, "fnt_ja_papyrus");

    
    writerCache.objPapdate = findObjectIndex(dw, "obj_papdate");

    
    writerCache.scrTexttype = findScriptCodeId(ctx, "SCR_TEXTTYPE");
    writerCache.scrNewline = findScriptCodeId(ctx, "SCR_NEWLINE");
    writerCache.scrReplaceButtonsPc = findScriptCodeId(ctx, "scr_replace_buttons_pc");
    writerCache.scrGetbuttonsprite = findScriptCodeId(ctx, "scr_getbuttonsprite");
    writerCache.scrSetfont = findScriptCodeId(ctx, "scr_setfont");
}


static inline char nativeStringCharAtBuf(const char* str, int32_t strLen, int32_t pos) {
    pos--; 
    if (0 > pos || pos >= strLen) return '\0';
    return str[pos];
}


static void native_objBaseWriter_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
#ifdef PSP
    u64 _wrT0; sceRtcGetCurrentTick(&_wrT0);
#endif
    Renderer* renderer = runner->renderer;
    if (renderer == nullptr) return;

    DataWin* dw = ctx->dataWin;

    
    int32_t vtext = selfInt(inst, writerCache.vtext);
    GMLReal writingxend = selfReal(inst, writerCache.writingxend);
    GMLReal vspacing = selfReal(inst, writerCache.vspacing);
    GMLReal writingx = selfReal(inst, writerCache.writingx);
    GMLReal writingy = selfReal(inst, writerCache.writingy);
    int32_t stringpos = selfInt(inst, writerCache.stringpos);
    const char* originalstring = selfString(inst, writerCache.originalstring);
    int32_t mycolor = selfInt(inst, writerCache.mycolor);
    int32_t myfont = selfInt(inst, writerCache.myfont);
    GMLReal shake = selfReal(inst, writerCache.shake);
    GMLReal spacing = selfReal(inst, writerCache.spacing);
    GMLReal htextscale = selfReal(inst, writerCache.htextscale);
    GMLReal vtextscale = selfReal(inst, writerCache.vtextscale);

    
    bool papdateExists = (writerCache.objPapdate >= 0 && findInstanceByObject(runner, writerCache.objPapdate) != NULL);

    
    static BuiltinFunc cachedInstanceExists = NULL;
    if (!cachedInstanceExists) cachedInstanceExists = VMBuiltins_find("instance_exists");

    GMLReal myx, myy;
    if (vtext) {
        myx = writingxend - vspacing;
    } else {
        myx = writingx;
    }
    myy = writingy;
    Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
    Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));

    int32_t halfsize = 0;
    const char* language = globalString(ctx, writerCache.gLanguage);
    bool isEnglish = (strcmp(language, "en") == 0);
    bool isJapanese = (strcmp(language, "ja") == 0);

    
    
    char* ownedOriginalString = nullptr;
    int32_t origStrLen = (int32_t)strlen(originalstring);

    for (int32_t n = 1; stringpos >= n; n++) {
        char ch = nativeStringCharAtBuf(originalstring, origStrLen, n);
        if (ch == '\0') break;

        
        
        
        if (ch >= ' ' && ch != '/' && ch != '\\' && ch != '^' && ch != '&' &&
            ch != 'z' && ch != '*' && ch != '>' && ch != '%' &&
            isEnglish && !vtext && (int32_t)shake < 39) {
            GMLReal hs = halfsize ? 0.5 : 1.0;
            GMLReal offsetx = 0, offsety = 0;

            
            if (myx > writingxend) {
                myx = writingx;
                myy += vspacing;
            }

            GMLReal letterx = myx;

            
            if (halfsize) offsety += (vspacing * 0.33);

            
            if ((int32_t)globalReal(ctx, writerCache.gTyper) == 18) {
                if (ch=='l'||ch=='i') letterx += 2;
                if (ch=='I'||ch=='!'||ch=='.') letterx += 2;
                if (ch=='S') letterx += 1;
                if (ch=='?') letterx += 2;
                if (ch=='D'||ch=='A') letterx += 1;
                if (ch=='\'') letterx += 1;
            }

            // Standard random shake (shake > 0 && shake < 39)
            if ((int32_t)shake != 0) {
                offsetx += ((GMLReal)rand() / (GMLReal)RAND_MAX) * shake - (shake / 2.0f);
                offsety += ((GMLReal)rand() / (GMLReal)RAND_MAX) * shake - (shake / 2.0f);
            }

            float drawX = (float)GMLReal_round(letterx + offsetx);
            float drawY = (float)GMLReal_round(myy + offsety);

            char letterStr[2] = { ch, '\0' };
            renderer->drawColor = (uint32_t)mycolor;
            renderer->drawFont = myfont;
            renderer->drawHalign = 0;
            renderer->drawValign = 0;
            renderer->vtable->drawText(renderer, letterStr,
                drawX, drawY,
                (float)(htextscale * hs), (float)(vtextscale * hs), 0.f);

            letterx += spacing;
            // Font-specific kerning
            if (myfont == writerCache.fntComicsans) {
                if (ch=='w'||ch=='m') letterx += 2;
                else if (ch=='i'||ch=='l') letterx -= 2;
                else if (ch=='s'||ch=='j') letterx -= 1;
            } else if (myfont == writerCache.fntPapyrus) {
                switch(ch) {
                    case 'D': case 'C': case 'A': case 'H': case 'B': case 'G': letterx+=1; break;
                    case 'Q': letterx+=3; break; case 'M': letterx+=1; break; case 'O': case 'W': letterx+=2; break;
                    case 'L': case 'T': case 'J': case 'F': letterx-=1; break;
                    case 'P': case 'R': letterx-=2; break;
                    case '.': case '!': case '?': letterx-=3; break;
                    case 'I': letterx-=6; break; case '\'': letterx-=6; break;
                }
            }
            
            

            
            if (halfsize)
                myx = GMLReal_round(myx + ((letterx - myx) / 2.0));
            else
                myx = letterx;
            continue;
        }

        if (ch == '^' && nativeStringCharAtBuf(originalstring, origStrLen, n + 1) != '0') {
            
            n++;
        } else if (ch == '\\') {
            n++;
            ch = nativeStringCharAtBuf(originalstring, origStrLen, n);
            
            bool handled = true;
            switch (ch) {
                case 'R': mycolor = 255; break;
                case 'G': mycolor = 65280; break;
                case 'W': mycolor = 16777215; break;
                case 'Y': mycolor = 65535; break;
                case 'X': mycolor = 0; break;
                case 'B': mycolor = 16711680; break;
                case 'O': mycolor = 4235519; break;
                case 'L': mycolor = 16629774; break;
                case 'P': mycolor = 16711935; break;
                case 'p': mycolor = 13941759; break;
                default: handled = false; break;
            }
            if (handled) {
                
            } else if (ch == 'C') {
                Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
            } else if (ch == 'M') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, origStrLen, n);
                GMLReal val = 0.0;
                if (ch != '\0') {
                    char buf[2] = { ch, '\0' };
                    val = GMLReal_strtod(buf, nullptr);
                }
                
                globalArraySet(ctx, writerCache.gFlag, 20, RValue_makeReal(val));
            } else if (ch == 'E') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, origStrLen, n);
                GMLReal val = 0.0;
                if (ch != '\0') {
                    char buf[2] = { ch, '\0' };
                    val = GMLReal_strtod(buf, nullptr);
                }
                globalSet(ctx, writerCache.gFaceemotion, RValue_makeReal(val));
            } else if (ch == 'F') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, origStrLen, n);
                GMLReal val = 0.0;
                if (ch != '\0') {
                    char buf[2] = { ch, '\0' };
                    val = GMLReal_strtod(buf, nullptr);
                }
                globalSet(ctx, writerCache.gFacechoice, RValue_makeReal(val));
                globalSet(ctx, writerCache.gFacechange, RValue_makeReal(1.0));
            } else if (ch == 'S') {
                n++;
            } else if (ch == 'T') {
                n++;
                char newtyper = nativeStringCharAtBuf(originalstring, origStrLen, n);
                if (newtyper == '-') {
                    halfsize = 1;
                } else if (newtyper == '+') {
                    halfsize = 0;
                } else {
                    int32_t typerVal = 0;
                    bool setTyper = true;
                    if (newtyper == 'T') typerVal = 4;
                    else if (newtyper == 't') typerVal = 48;
                    else if (newtyper == '0') typerVal = 5;
                    else if (newtyper == 'S') typerVal = 10;
                    else if (newtyper == 'F') typerVal = 16;
                    else if (newtyper == 's') typerVal = 17;
                    else if (newtyper == 'P') typerVal = 18;
                    else if (newtyper == 'M') typerVal = 27;
                    else if (newtyper == 'U') typerVal = 37;
                    else if (newtyper == 'A') typerVal = 47;
                    else if (newtyper == 'a') typerVal = 60;
                    else if (newtyper == 'R') typerVal = 76;
                    else setTyper = false;

                    if (setTyper) {
                        globalSet(ctx, writerCache.gTyper, RValue_makeReal((GMLReal) typerVal));
                    }

                    
                    GMLReal currentTyper = globalReal(ctx, writerCache.gTyper);
                    RValue scrArg = RValue_makeReal(currentTyper);
                    RValue scrResult = VM_callCodeIndex(ctx, writerCache.scrTexttype, &scrArg, 1);
                    RValue_free(&scrResult);

                    globalSet(ctx, writerCache.gFacechange, RValue_makeReal(1.0));
                }
            } else if (ch == 'z') {
                n++;
                char symCh = nativeStringCharAtBuf(originalstring, origStrLen, n);
                GMLReal sym = 0.0;
                if (symCh != '\0') {
                    char buf[2] = { symCh, '\0' };
                    sym = GMLReal_strtod(buf, nullptr);
                }
                if ((int32_t) sym == 4) {
                    int32_t symS = 862;
                    GMLReal rshake = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                    GMLReal rshake2 = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                    Renderer_drawSpriteExt(renderer, symS, 0,
                        (float) (myx + (rshake - (shake / 2.0))),
                        (float) (myy + 10.0 + (rshake2 - (shake / 2.0))),
                        2.0f, 2.0f, 0.0f, 0xFFFFFF, 1.0f);
                }
            } else if (ch == '*') {
                n++;
                ch = nativeStringCharAtBuf(originalstring, origStrLen, n);
                int32_t icontype = 0;
                if (myfont == writerCache.fntPapyrus || myfont == writerCache.fntJaPapyrusBtl) {
                    icontype = 1;
                }
                
                char chStr[2] = { ch, '\0' };
                RValue getbtnArgs[2] = { RValue_makeString(chStr), RValue_makeReal((GMLReal) icontype) };
                RValue spriteResult = VM_callCodeIndex(ctx, writerCache.scrGetbuttonsprite, getbtnArgs, 2);
                int32_t sprite = RValue_toInt32(spriteResult);
                RValue_free(&spriteResult);

                if (sprite != -4) {
                    GMLReal spritex = myx;
                    GMLReal spritey = myy;
                    if (shake > 38) {
                        if ((int32_t) shake == 39) {
                            setDirection(inst, inst->direction + 10);
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                        } else if ((int32_t) shake == 40) {
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                        } else if ((int32_t) shake == 41) {
                            setDirection(inst, inst->direction + (10.0 * n));
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                            setDirection(inst, inst->direction - (10.0 * n));
                        } else if ((int32_t) shake == 42) {
                            setDirection(inst, inst->direction + (20.0 * n));
                            spritex += inst->hspeed;
                            spritey += inst->vspeed;
                            setDirection(inst, inst->direction - (20.0 * n));
                        } else if ((int32_t) shake == 43) {
                            setDirection(inst, inst->direction + (30.0 * n));
                            spritex += ((inst->hspeed * 0.7) + 10);
                            spritey += (inst->vspeed * 0.7);
                            setDirection(inst, inst->direction - (30.0 * n));
                        }
                    } else if (!papdateExists) {
                        GMLReal rshake = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                        GMLReal rshake2 = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                        spritex += (rshake - (shake / 2.0));
                        spritey += (rshake2 - (shake / 2.0));
                    }
                    GMLReal iconScale = 1.0;
                    if (myfont == writerCache.fntMain || myfont == writerCache.fntJaMain) {
                        iconScale = 2.0;
                    }
                    if (myfont == writerCache.fntMain || myfont == writerCache.fntMaintext) {
                        spritey += (1.0 * iconScale);
                    }
                    if (myfont == writerCache.fntJaPapyrusBtl) {
                        spritex -= 1;
                    }
                    if (myfont == writerCache.fntPapyrus && icontype == 1) {
                        int32_t sprHeight = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].height : 0;
                        spritey += GMLReal_floor((16.0 - sprHeight) / 2.0);
                    }
                    if (vtext) {
                        int32_t sprWidth = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].width : 0;
                        Renderer_drawSpriteExt(renderer, sprite, 0,
                            (float) (spritex - sprWidth), (float) spritey,
                            (float) iconScale, (float) iconScale, 0.0f, 0xFFFFFF, 1.0f);
                        int32_t sprHeight = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].height : 0;
                        myy += ((sprHeight + 1) * iconScale);
                    } else {
                        Renderer_drawSpriteExt(renderer, sprite, 0,
                            (float) spritex, (float) spritey,
                            (float) iconScale, (float) iconScale, 0.0f, 0xFFFFFF, 1.0f);
                        int32_t sprWidth = (sprite >= 0 && dw->sprt.count > (uint32_t) sprite) ? (int32_t) dw->sprt.sprites[sprite].width : 0;
                        myx += ((sprWidth + 1) * iconScale);
                    }
                }
            } else if (ch == '>') {
                n++;
                char choiceCh = nativeStringCharAtBuf(originalstring, origStrLen, n);
                GMLReal choiceindex = 0.0;
                if (choiceCh != '\0') {
                    char buf[2] = { choiceCh, '\0' };
                    choiceindex = GMLReal_strtod(buf, nullptr);
                }
                if ((int32_t) choiceindex == 1) {
                    myx = 196;
                } else {
                    myx = 100;
                    if (myfont == writerCache.fntJaComicsansBig) {
                        myx += 11;
                    }
                }
                
                int32_t viewCurrent = runner->viewCurrent;
                int32_t viewWview = 0;
                if (viewCurrent >= 0 && 8 > viewCurrent) {
                    viewWview = (int32_t) runner->currentRoom->views[viewCurrent].viewWidth;
                }
                if (viewWview == 640) {
                    myx *= 2;
                }
                
                int32_t viewXview = 0;
                if (viewCurrent >= 0 && 8 > viewCurrent) {
                    viewXview = (int32_t) runner->currentRoom->views[viewCurrent].viewX;
                }
                myx += viewXview;
            }
        } else if (ch == '&') {
            
            Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
            Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));
            
            RValue newlineResult = VM_callCodeIndex(ctx, writerCache.scrNewline, nullptr, 0);
            RValue_free(&newlineResult);
            
            myx = selfReal(inst, writerCache.myx);
            myy = selfReal(inst, writerCache.myy);
        } else if (ch == '/') {
            int32_t halt = 1;
            char nextch = nativeStringCharAtBuf(originalstring, origStrLen, n + 1);
            if (nextch == '%') {
                halt = 2;
            } else if (nextch == '^' && nativeStringCharAtBuf(originalstring, origStrLen, n + 2) != '0') {
                halt = 4;
            } else if (nextch == '*') {
                halt = 6;
            }
            Instance_setSelfVar(inst, writerCache.halt, RValue_makeReal((GMLReal) halt));
            break;
        } else if (ch == '%') {
            if (nativeStringCharAtBuf(originalstring, origStrLen, n + 1) == '%') {
                
                Runner_destroyInstance(runner, inst);
                break;
            }
            int32_t stringno = selfInt(inst, writerCache.stringno);
            stringno++;
            Instance_setSelfVar(inst, writerCache.stringno, RValue_makeReal((GMLReal) stringno));

            
            RValue mystringVal = selfArrayGet(inst, writerCache.mystring, stringno);
            RValue replaceArgs[1] = { mystringVal };
            RValue replaceResult = VM_callCodeIndex(ctx, writerCache.scrReplaceButtonsPc, replaceArgs, 1);

            
            if (ownedOriginalString != nullptr) {
                free(ownedOriginalString);
                ownedOriginalString = nullptr;
            }

            
            Instance_setSelfVar(inst, writerCache.originalstring, replaceResult);
            
            originalstring = selfString(inst, writerCache.originalstring);
            RValue_free(&replaceResult);

            stringpos = 0;
            Instance_setSelfVar(inst, writerCache.stringpos, RValue_makeReal(0.0));
            myx = writingx;
            myy = writingy;
            inst->alarm[0] = selfInt(inst, writerCache.textspeed);
            break;
        } else {
            
            char myletter = nativeStringCharAtBuf(originalstring, origStrLen, n);
            if (myletter == '^') {
                n++;
                myletter = nativeStringCharAtBuf(originalstring, origStrLen, n);
            }
            if (!vtext && myx > writingxend) {
                
                Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
                Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));
                
                RValue newlineResult = VM_callCodeIndex(ctx, writerCache.scrNewline, nullptr, 0);
                RValue_free(&newlineResult);
                myx = selfReal(inst, writerCache.myx);
                myy = selfReal(inst, writerCache.myy);
            }
            GMLReal letterx = myx;
            GMLReal offsetx = 0;
            GMLReal offsety = 0;
            GMLReal halfscale = 1.0;
            if (halfsize) {
                halfscale = 0.5;
                if (vtext) {
                    offsetx += (vspacing * 0.33);
                } else {
                    offsety += (vspacing * 0.33);
                }
            }
            if (isEnglish) {
                if ((int32_t) globalReal(ctx, writerCache.gTyper) == 18) {
                    if (myletter == 'l' || myletter == 'i') letterx += 2;
                    if (myletter == 'I') letterx += 2;
                    if (myletter == '!') letterx += 2;
                    if (myletter == '.') letterx += 2;
                    if (myletter == 'S') letterx += 1;
                    if (myletter == '?') letterx += 2;
                    if (myletter == 'D') letterx += 1;
                    if (myletter == 'A') letterx += 1;
                    if (myletter == '\'') letterx += 1;
                }
            } else if (isJapanese) {
                if (vtext && (myfont == writerCache.fntJaPapyrus || myfont == writerCache.fntJaPapyrusBtl)) {
                    char myletterStr[2] = { myletter, '\0' };
                    bool isBracket = (strcmp(myletterStr, "\xe3\x80\x8c") == 0 || strcmp(myletterStr, "\xe3\x80\x8e") == 0);
                    // Note: This check won't work for multi-byte chars with single char buffer
                    
                    
                    if ((int32_t) myy == (int32_t) writingy && isBracket) {
                        
                        RValue swArgs[1] = { RValue_makeString(myletterStr) };
                        RValue sw = callBuiltin(ctx, "string_width", swArgs, 1);
                        myy -= GMLReal_round((RValue_toReal(sw) / 2.0) * htextscale * halfscale);
                        RValue_free(&sw);
                    }
                } else if (myfont == writerCache.fntJaMaintext || myfont == writerCache.fntJaMain) {
                    GMLReal unit = htextscale * halfscale;
                    if (myfont == writerCache.fntJaMain) {
                        unit *= 2;
                    }
                    
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
                    int32_t ordVal = (int32_t) (unsigned char) myletter;
                    if (ordVal < 1024 || ordVal == 8211) {
                        if (n > 1) {
                            char lastchChar = nativeStringCharAtBuf(originalstring, origStrLen, n - 1);
                            int32_t lastch = (int32_t) (unsigned char) lastchChar;
                            if (lastch >= 1024 && lastch < 65281 && lastch != 8211 && lastch != 12288) {
                                letterx += unit;
                            }
                        }
#pragma GCC diagnostic pop
                    }
                }
            }

            
            RValue setfontArg = RValue_makeReal((GMLReal) myfont);
            RValue setfontResult = VM_callCodeIndex(ctx, writerCache.scrSetfont, &setfontArg, 1);
            RValue_free(&setfontResult);

            
            renderer->drawColor = (uint32_t) mycolor;

            GMLReal angle = vtext ? -90.0 : 0.0;

            if (shake > 38) {
                if ((int32_t) shake == 39) {
                    setDirection(inst, inst->direction + 10);
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                } else if ((int32_t) shake == 40) {
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                } else if ((int32_t) shake == 41) {
                    setDirection(inst, inst->direction + (10.0 * n));
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                    setDirection(inst, inst->direction - (10.0 * n));
                } else if ((int32_t) shake == 42) {
                    setDirection(inst, inst->direction + (20.0 * n));
                    offsetx += inst->hspeed;
                    offsety += inst->vspeed;
                    setDirection(inst, inst->direction - (20.0 * n));
                } else if ((int32_t) shake == 43) {
                    setDirection(inst, inst->direction + (30.0 * n));
                    offsetx += ((inst->hspeed * 0.7) + 10);
                    offsety += (inst->vspeed * 0.7);
                    setDirection(inst, inst->direction - (30.0 * n));
                }
            } else {
                GMLReal rshake = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                GMLReal rshake2 = ((GMLReal) rand() / (GMLReal) RAND_MAX) * shake;
                offsetx += (rshake - (shake / 2.0));
                offsety += (rshake2 - (shake / 2.0));
            }

            GMLReal finalx = GMLReal_round(letterx + offsetx);
            GMLReal finaly = GMLReal_round(myy + offsety);

            
            
            {
                char letterStr[2] = { myletter, '\0' };
                float xsc = (float)(htextscale * halfscale);
                float ysc = (float)(vtextscale * halfscale);
                renderer->drawFont = (int32_t) myfont;
                renderer->drawHalign = 0;
                renderer->drawValign = 0;
                renderer->vtable->drawText(renderer, letterStr, (float)finalx, (float)finaly, xsc, ysc, (float)angle);
            }

            letterx += spacing;

            if (isEnglish) {
                if (myfont == writerCache.fntComicsans) {
                    if (myletter == 'w') letterx += 2;
                    if (myletter == 'm') letterx += 2;
                    if (myletter == 'i') letterx -= 2;
                    if (myletter == 'l') letterx -= 2;
                    if (myletter == 's') letterx -= 1;
                    if (myletter == 'j') letterx -= 1;
                } else if (myfont == writerCache.fntPapyrus) {
                    if (myletter == 'D') letterx += 1;
                    if (myletter == 'Q') letterx += 3;
                    if (myletter == 'M') letterx += 1;
                    if (myletter == 'L') letterx -= 1;
                    if (myletter == 'K') letterx -= 1;
                    if (myletter == 'C') letterx += 1;
                    if (myletter == '.') letterx -= 3;
                    if (myletter == '!') letterx -= 3;
                    if (myletter == 'O' || myletter == 'W') letterx += 2;
                    if (myletter == 'I') letterx -= 6;
                    if (myletter == 'T') letterx -= 1;
                    if (myletter == 'P') letterx -= 2;
                    if (myletter == 'R') letterx -= 2;
                    if (myletter == 'A') letterx += 1;
                    if (myletter == 'H') letterx += 1;
                    if (myletter == 'B') letterx += 1;
                    if (myletter == 'G') letterx += 1;
                    if (myletter == 'F') letterx -= 1;
                    if (myletter == '?') letterx -= 3;
                    if (myletter == '\'') letterx -= 6;
                    if (myletter == 'J') letterx -= 1;
                }
            } else if (isJapanese) {
                // Note: Japanese text support requires proper UTF-8 multi-byte character handling.
                // The single-byte char approach here only handles ASCII correctly. For multi-byte
                // characters (ord >= 256), the GML original uses string_char_at which returns full
                // Unicode codepoints. For now, single-byte characters always fall through to the
                // "< 1024" branch since unsigned char maxes at 255.
                // ordVal will only be 0-255 for single-byte chars; the >= 65377 / == 8211 branches
                // are unreachable but kept for parity with the GML original (future UTF-8 support)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
                int32_t ordVal = (int32_t) (unsigned char) myletter;
                if (vtext) {
                    // string_width(myletter)
                    char letterStr[2] = { myletter, '\0' };
                    RValue swArgs[1] = { RValue_makeString(letterStr) };
                    RValue sw = callBuiltin(ctx, "string_width", swArgs, 1);
                    myy += GMLReal_round(RValue_toReal(sw) * htextscale * halfscale);
                    RValue_free(&sw);
                } else if (myletter == ' ' || ordVal >= 65377) {
                    letterx -= GMLReal_floor(spacing / 2.0);
                } else if (1024 > ordVal || ordVal == 8211) {
                    if (myfont == writerCache.fntJaComicsans || myfont == writerCache.fntJaComicsansBig) {
                        letterx -= GMLReal_floor(spacing * 0.3);
                    } else {
                        letterx -= GMLReal_floor(spacing * 0.4);
                    }
                }
#pragma GCC diagnostic pop
            }

            if (!vtext) {
                if (halfsize) {
                    myx = GMLReal_round(myx + ((letterx - myx) / 2.0));
                } else {
                    myx = letterx;
                }
            }
        }
    }

    // Write back myx, myy and mycolor to the instance
    Instance_setSelfVar(inst, writerCache.myx, RValue_makeReal(myx));
    Instance_setSelfVar(inst, writerCache.myy, RValue_makeReal(myy));
    Instance_setSelfVar(inst, writerCache.mycolor, RValue_makeReal((GMLReal) mycolor));

    if (ownedOriginalString != nullptr) {
        free(ownedOriginalString);
    }
#ifdef PSP
    { u64 _wrT1; sceRtcGetCurrentTick(&_wrT1); g_writerTimeUs += (uint32_t)(_wrT1 - _wrT0); g_writerCalls++; }
#endif
}

// =====================================================================
// NATIVE: blt_minihelix_Step_0
// GML: hspeed = sin(h / 5) * 8;  if (r == 0) hspeed = -sin(h / 5) * 8;  h += 1;
// =====================================================================
static struct {
    int32_t h;   // self.h
    int32_t r;   // self.r
    bool ready;
} minihelixCache = { .ready = false };

static void initMinihelixCache(DataWin* dw) {
    minihelixCache.h = findSelfVarId(dw, "h");
    minihelixCache.r = findSelfVarId(dw, "r");
    minihelixCache.ready = (minihelixCache.h >= 0 && minihelixCache.r >= 0);
}

static void native_bltMinihelix_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    if (!minihelixCache.ready) return;

    GMLReal h = RValue_toReal(Instance_getSelfVar(inst, minihelixCache.h));
    GMLReal r = RValue_toReal(Instance_getSelfVar(inst, minihelixCache.r));

    inst->hspeed = (float)(sinf((float)(h / 5.0)) * 8.0);
    if (r == 0)
        inst->hspeed = (float)(-sinf((float)(h / 5.0)) * 8.0);

    Instance_setSelfVar(inst, minihelixCache.h, RValue_makeReal(h + 1.0));
}

// =====================================================================
// NATIVE: blt_parent_Step_2
// GML: if (global.turntimer < 0) instance_destroy();
// =====================================================================
static struct {
    int32_t turntimer; // global.turntimer varID
    bool ready;
} bltParentCache = { .ready = false };

static void initBltParentCache(VMContext* ctx) {
    bltParentCache.turntimer = findGlobalVarId(ctx, "turntimer");
    bltParentCache.ready = (bltParentCache.turntimer >= 0);
}

static void native_bltParent_Step2(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!bltParentCache.ready) return;

    GMLReal turntimer = RValue_toReal(ctx->globalVars[bltParentCache.turntimer]);
    if (turntimer < 0) {
        Runner_destroyInstance(runner, inst);
    }
}

// =====================================================================
// NATIVE: bulletgenparent_Step_2
// GML: if (global.turntimer < 1) { global.turntimer = -1; global.mnfight = 3; instance_destroy(); }
// =====================================================================
static struct {
    int32_t mnfight; // global.mnfight
    bool ready;
} bulletgenCache = { .ready = false };

static void initBulletgenCache(VMContext* ctx) {
    bulletgenCache.mnfight = findGlobalVarId(ctx, "mnfight");
    // turntimer already cached in bltParentCache
    bulletgenCache.ready = (bulletgenCache.mnfight >= 0 && bltParentCache.ready);
}

static void native_bulletgenparent_Step2(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!bulletgenCache.ready) return;
    GMLReal tt = RValue_toReal(ctx->globalVars[bltParentCache.turntimer]);
    if (tt < 1.0) {
        RValue_free(&ctx->globalVars[bltParentCache.turntimer]);
        ctx->globalVars[bltParentCache.turntimer] = RValue_makeReal(-1.0);
        RValue_free(&ctx->globalVars[bulletgenCache.mnfight]);
        ctx->globalVars[bulletgenCache.mnfight] = RValue_makeReal(3.0);
        Runner_destroyInstance(runner, inst);
    }
}

// =====================================================================
// NATIVE: hpname_Step_0
// GML: if (instance_exists(155)) depth = obj_battlecontroller.depth;
// =====================================================================
static struct {
    int32_t bcObjIdx; // object index for obj_battlecontroller (155)
    bool ready;
} hpnameCache = { .ready = false };

static void initHpnameCache(DataWin* dw) {
    hpnameCache.bcObjIdx = findObjectIndex(dw, "obj_battlecontroller");
    hpnameCache.ready = (hpnameCache.bcObjIdx >= 0);
}

static void native_hpname_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!hpnameCache.ready) return;
    // Find obj_battlecontroller instance
    int32_t count = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < count; i++) {
        Instance* bc = runner->instances[i];
        if (bc->active && bc->objectIndex == hpnameCache.bcObjIdx) {
            inst->depth = bc->depth;
            return;
        }
    }
}

// =====================================================================
// NATIVE: battlecontroller_Step_1
// GML: if (control_check(0) == 1) event_user(0);
// =====================================================================
static void native_battlecontroller_Step1(VMContext* ctx, Runner* runner, Instance* inst) {
    // control_check(0) = keyboard_check(vk_confirm) — check via VMBuiltins
    RValue arg = RValue_makeReal(0);
    BuiltinFunc controlCheck = VMBuiltins_find("control_check");
    if (controlCheck) {
        RValue result = controlCheck(ctx, &arg, 1);
        if (RValue_toReal(result) == 1.0) {
            // event_user(0) = EVENT_OTHER, subtype = OTHER_USER0 + 0 = 10
            Runner_executeEvent(runner, inst, 7, 10);
        }
        RValue_free(&result);
    }
}

// =====================================================================
// NATIVE: blt_chasefire2_Step_0
// GML:
//   if (instance_exists(616)) {
//     if (blt_handbullet1.path_position == 1 && goof == 0) {
//       move_towards_point(obj_heart.x+2, obj_heart.y+2, 0.6);
//       goof = 1; friction = -0.1;
//     }
//   }
//   if (y > (global.idealborder[3] + 4)) instance_destroy();
// =====================================================================
static struct {
    int32_t goofId;         // self.goof
    int32_t handbullet1Obj; // blt_handbullet1 object index
    int32_t heartObj;       // obj_heart object index
    int32_t lborderObj;     // obj_lborder object index
    int32_t rborderObj;     // obj_rborder object index
    int32_t idealborderId;  // global.idealborder varID
    bool ready;
} chasefire2Cache = { .ready = false };

static void initChasefire2Cache(VMContext* ctx, DataWin* dw) {
    chasefire2Cache.goofId = findSelfVarId(dw, "goof");
    chasefire2Cache.handbullet1Obj = findObjectIndex(dw, "blt_handbullet1");
    chasefire2Cache.heartObj = findObjectIndex(dw, "obj_heart");
    chasefire2Cache.lborderObj = findObjectIndex(dw, "obj_lborder");
    chasefire2Cache.rborderObj = findObjectIndex(dw, "obj_rborder");
    chasefire2Cache.idealborderId = findGlobalVarId(ctx, "idealborder");
    chasefire2Cache.ready = (chasefire2Cache.goofId >= 0 && chasefire2Cache.heartObj >= 0 && chasefire2Cache.idealborderId >= 0);
}

static Instance* findInstanceByObject(Runner* runner, int32_t objIdx) {
    if (objIdx < 0 || objIdx >= runner->instancesByObjMax || runner->instancesByObjInclParent == NULL) return NULL;

    Instance** list = runner->instancesByObjInclParent[objIdx];
    int32_t n = (int32_t)arrlen(list);
    for (int32_t i = 0; i < n; i++) {
        if (list[i]->active) return list[i];
    }
    return NULL;
}

static void native_chasefire2_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!chasefire2Cache.ready) return;

    // if (instance_exists(blt_handbullet1))
    if (chasefire2Cache.handbullet1Obj >= 0) {
        Instance* hb1 = findInstanceByObject(runner, chasefire2Cache.handbullet1Obj);
        if (hb1) {
            // if (blt_handbullet1.path_position == 1 && goof == 0)
            GMLReal goof = RValue_toReal(Instance_getSelfVar(inst, chasefire2Cache.goofId));
            if (hb1->pathPosition == 1.0f && goof == 0) {
                // move_towards_point(obj_heart.x+2, obj_heart.y+2, 0.6)
                Instance* heart = findInstanceByObject(runner, chasefire2Cache.heartObj);
                if (heart) {
                    float dx = (heart->x + 2.0f) - inst->x;
                    float dy = (heart->y + 2.0f) - inst->y;
                    float dist = GMLReal_sqrt(dx * dx + dy * dy);
                    if (dist > 0) {
                        inst->direction = atan2f(-dy, dx) * 180.0f / (float)M_PI;
                        if (inst->direction < 0) inst->direction += 360.0f;
                        inst->speed = 0.6f;
                        float rad = inst->direction * (float)M_PI / 180.0f;
                        inst->hspeed = 0.6f * cosf(rad);
                        inst->vspeed = -0.6f * sinf(rad);
                    }
                }
                Instance_setSelfVar(inst, chasefire2Cache.goofId, RValue_makeReal(1.0));
                inst->friction = -0.1f;
            }
        }
    }

    // if (y > (global.idealborder[3] + 4)) instance_destroy();
    // idealborder is a global array, index 3
    int64_t arrKey = ((int64_t)chasefire2Cache.idealborderId << 32) | 3u;
    ptrdiff_t arrIdx = hmgeti(ctx->globalArrayMap, arrKey);
    if (arrIdx >= 0) {
        GMLReal border3 = RValue_toReal(ctx->globalArrayMap[arrIdx].value);
        if (inst->y > border3 + 4.0f) {
            Runner_destroyInstance(runner, inst);
        }
    }
}

// =====================================================================
// NATIVE HELPER: draw_self_border_true (inlined)
// Used by ALL bullet Draw_0 scripts (blt_chasefire2, blt_minihelix, etc.)
// Clips sprite to battle box borders and draws visible part.
// =====================================================================
// Global per-frame heart cache (shared by ALL bullet collision checks)
static struct {
    Instance* inst;
    InstanceBBox bbox;
    uint64_t frame;
} heartCache = { .inst = NULL, .frame = UINT64_MAX };

static void updateHeartCache(Runner* runner) {
    if (heartCache.frame == (uint64_t)runner->frameCount) return;
    heartCache.frame = (uint64_t)runner->frameCount;
    heartCache.inst = findInstanceByObject(runner, chasefire2Cache.heartObj);
    if (heartCache.inst)
        heartCache.bbox = Collision_computeBBox(runner->dataWin, heartCache.inst);
    else
        heartCache.bbox.valid = false;
}

// Quick AABB collision check with cached heart
static inline bool checkHeartCollision(Runner* runner, Instance* inst) {
    updateHeartCache(runner);
    if (!heartCache.inst || !heartCache.bbox.valid) return false;
    InstanceBBox selfBBox = Collision_computeBBox(runner->dataWin, inst);
    return selfBBox.valid &&
        selfBBox.left < heartCache.bbox.right && selfBBox.right > heartCache.bbox.left &&
        selfBBox.top < heartCache.bbox.bottom && selfBBox.bottom > heartCache.bbox.top;
}

static inline GMLReal getGlobalArray(VMContext* ctx, int32_t varID, int32_t index) {
    int64_t k = ((int64_t)varID << 32) | (uint32_t)index;
    ptrdiff_t i = hmgeti(ctx->globalArrayMap, k);
    return (i >= 0) ? RValue_toReal(ctx->globalArrayMap[i].value) : 0.0;
}

// Per-frame cache for border positions (avoids 2× findInstanceByObject + 2× getGlobalArray per bullet)
static struct {
    float lbx, rbx, border2, border3;
    uint64_t frame;
} borderPosCache = { .frame = UINT64_MAX };

static void updateBorderPosCache(VMContext* ctx, Runner* runner) {
    if (borderPosCache.frame == (uint64_t)runner->frameCount) return;
    borderPosCache.frame = (uint64_t)runner->frameCount;
    Instance* lb = findInstanceByObject(runner, chasefire2Cache.lborderObj);
    Instance* rb = findInstanceByObject(runner, chasefire2Cache.rborderObj);
    borderPosCache.lbx = lb ? lb->x : 0;
    borderPosCache.rbx = rb ? rb->x : 320;
    borderPosCache.border2 = (float)getGlobalArray(ctx, chasefire2Cache.idealborderId, 2);
    borderPosCache.border3 = (float)getGlobalArray(ctx, chasefire2Cache.idealborderId, 3);
}

static void native_drawSelfBorder(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!chasefire2Cache.ready) { Renderer_drawSelf(runner->renderer, inst); return; }
    DataWin* dw = ctx->dataWin;
    Renderer* r = runner->renderer;
    if (!r || inst->spriteIndex < 0 || (uint32_t)inst->spriteIndex >= dw->sprt.count) return;

    Sprite* spr = &dw->sprt.sprites[inst->spriteIndex];
    float sw = (float)spr->width * inst->imageXscale;
    float sh = (float)spr->height * inst->imageYscale;

    // Get border positions — cached per frame (was 2× findInstanceByObject = O(N) per call!)
    updateBorderPosCache(ctx, runner);
    float lbx = borderPosCache.lbx;
    float rbx = borderPosCache.rbx;
    float border2 = borderPosCache.border2;
    float border3 = borderPosCache.border3;

    float l = 0, t = 0, w = sw, h = sh;
    float ll = (lbx - inst->x) + 1.0f;
    float tt = (border2 - inst->y) + 1.0f;
    float ww = (inst->x + w) - rbx - 1.0f;
    float hh = (inst->y + h) - border3 - 1.0f;

    if (ll > 0) l += ll;
    if (tt > 0) t += tt;
    if (ww > 0) w -= ww;
    if (hh > 0) h -= hh;

    w = GMLReal_round(w); h = GMLReal_round(h);
    l = GMLReal_round(l); t = GMLReal_round(t);

    if (w > 0 && h > 0 && l < w && t < h) {
        int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, inst->spriteIndex, (int32_t)inst->imageIndex);
        if (tpagIndex >= 0) {
            r->vtable->drawSpritePart(r, tpagIndex,
                (int32_t)l, (int32_t)t, (int32_t)(w - l), (int32_t)(h - t),
                inst->x + l, inst->y + t, 1.0f, 1.0f, 0xFFFFFF, inst->imageAlpha);
        }
    }
}

// =====================================================================
// NATIVE BUILTIN: scr_gettext(text_id, [arg1], [arg2], ...)
// Replaces the GML script which does:
//   ds_map_find_value → for loop with string_copy per char → replace \[X]
// The GML version is O(N) per call with malloc on every char. This is O(1) scan.
// =====================================================================
static struct {
    int32_t textDataEn;   // global.text_data_en (ds_map ID)
    int32_t textDataJa;   // global.text_data_ja
    int32_t language;      // global.language
    int32_t charname;      // global.charname
    int32_t gold;          // global.gold
    int32_t itemname;      // global.itemname (array)
    int32_t menucoord;     // global.menucoord (array)
    bool ready;
} gettextCache = { .ready = false };

static void initGettextCache(VMContext* ctx) {
    gettextCache.textDataEn = findGlobalVarId(ctx, "text_data_en");
    gettextCache.textDataJa = findGlobalVarId(ctx, "text_data_ja");
    gettextCache.language   = findGlobalVarId(ctx, "language");
    gettextCache.charname   = findGlobalVarId(ctx, "charname");
    gettextCache.gold       = findGlobalVarId(ctx, "gold");
    gettextCache.itemname   = findGlobalVarId(ctx, "itemname");
    gettextCache.menucoord  = findGlobalVarId(ctx, "menucoord");
    gettextCache.ready = (gettextCache.textDataEn >= 0);
}

static RValue native_scr_gettext(VMContext* ctx, RValue* args, int32_t argCount) {
    if (!gettextCache.ready || argCount < 1) return RValue_makeOwnedString(safeStrdup(""));

    // Get text_id string
    const char* textId = (args[0].type == RVALUE_STRING && args[0].string) ? args[0].string : "";

    // ds_map_find_value(global.text_data_en, text_id)
    BuiltinFunc dsMapFind = VMBuiltins_find("ds_map_find_value");
    if (!dsMapFind) return RValue_makeOwnedString(safeStrdup(""));

    RValue mapIdEn = ctx->globalVars[gettextCache.textDataEn];
    RValue dsArgs[2] = { mapIdEn, args[0] };
    RValue textVal = dsMapFind(ctx, dsArgs, 2);

    const char* text = "";
    if (textVal.type == RVALUE_STRING && textVal.string) text = textVal.string;
    else if (textVal.type == RVALUE_UNDEFINED) text = "";
    else { char* ts = RValue_toString(textVal); RValue_free(&textVal); textVal = RValue_makeOwnedString(ts); text = ts; }

    // Check Japanese
    if (gettextCache.textDataJa >= 0 && gettextCache.language >= 0) {
        RValue langVal = ctx->globalVars[gettextCache.language];
        if (langVal.type == RVALUE_STRING && langVal.string && strcmp(langVal.string, "ja") == 0) {
            RValue mapIdJa = ctx->globalVars[gettextCache.textDataJa];
            RValue dsArgsJa[2] = { mapIdJa, args[0] };
            RValue jaVal = dsMapFind(ctx, dsArgsJa, 2);
            if (jaVal.type == RVALUE_STRING && jaVal.string) {
                RValue_free(&textVal);
                textVal = jaVal;
                text = jaVal.string;
            } else {
                RValue_free(&jaVal);
            }
        }
    }

    // Replace \[X] patterns — direct C scan, no malloc per char
    size_t len = strlen(text);
    if (len < 4) {
        // Too short for any \[X] pattern — return as-is
        char* result = safeStrdup(text);
        RValue_free(&textVal);
        return RValue_makeOwnedString(result);
    }

    // Build result with replacements
    char* result = safeMalloc(len * 4 + 64); // worst case: every \[X] replaced with long string
    size_t outPos = 0;

    for (size_t i = 0; i < len; i++) {
        // Check for \[X] pattern: backslash, bracket, char, bracket
        if (i + 3 < len && text[i] == '\\' && text[i+1] == '[' && text[i+3] == ']') {
            char sel = text[i+2];
            const char* replace = "";
            char numBuf[32];

            if (sel == 'C' && gettextCache.charname >= 0) {
                replace = globalString(ctx, gettextCache.charname);
            } else if (sel == 'I' && gettextCache.itemname >= 0 && gettextCache.menucoord >= 0) {
                // \[I] = global.itemname[global.menucoord[1]]
                int32_t menuIdx = (int32_t)getGlobalArray(ctx, gettextCache.menucoord, 1);
                int64_t k = ((int64_t)gettextCache.itemname << 32) | (uint32_t)menuIdx;
                ptrdiff_t idx = hmgeti(ctx->globalArrayMap, k);
                if (idx >= 0 && ctx->globalArrayMap[idx].value.type == RVALUE_STRING &&
                    ctx->globalArrayMap[idx].value.string != NULL) {
                    replace = ctx->globalArrayMap[idx].value.string;
                }
            } else if (sel == 'G' && gettextCache.gold >= 0) {
                snprintf(numBuf, sizeof(numBuf), "%d", (int)globalReal(ctx, gettextCache.gold));
                replace = numBuf;
            } else if (sel >= '1' && sel <= '9') {
                int argIdx = sel - '0';
                if (argIdx < argCount && args[argIdx].type == RVALUE_STRING && args[argIdx].string) {
                    replace = args[argIdx].string;
                } else if (argIdx < argCount) {
                    char* ts = RValue_toString(args[argIdx]);
                    size_t rl = strlen(ts);
                    memcpy(result + outPos, ts, rl);
                    outPos += rl;
                    free(ts);
                    i += 3; // skip \[X]
                    continue;
                }
            }

            size_t rl = strlen(replace);
            memcpy(result + outPos, replace, rl);
            outPos += rl;
            i += 3; // skip \[X]
        } else {
            result[outPos++] = text[i];
        }
    }
    result[outPos] = '\0';

    RValue_free(&textVal);
    return RValue_makeOwnedString(result);
}

// =====================================================================
// NATIVE: obj_ct_fallobj_Step_0  (117× per frame in settings!)
// GML:
//   if (y > 250) instance_destroy();
//   siner += 1;
//   x += sin(siner/5) * sinerfactor;
//   y += cos(siner/6) * sinerfactor;
//   image_angle += rotspeed;
// =====================================================================
static struct {
    int32_t siner, sinerfactor, rotspeed;
    bool ready;
} fallobjCache = { .ready = false };

static void initFallobjCache(DataWin* dw) {
    fallobjCache.siner = findSelfVarId(dw, "siner");
    fallobjCache.sinerfactor = findSelfVarId(dw, "sinerfactor");
    fallobjCache.rotspeed = findSelfVarId(dw, "rotspeed");
    fallobjCache.ready = (fallobjCache.siner >= 0 && fallobjCache.sinerfactor >= 0 && fallobjCache.rotspeed >= 0);
}

// NATIVE: obj_ct_fallobj_Create_0 — sets initial random motion/position.
// Nativized so we can skip VM dispatch on the ~1-per-frame creation in settings room.
// GML:
//   x = random(room_width);
//   gravity = 0.02; vspeed = 1; image_alpha = 0.5;
//   rotspeed = choose(1, -1) * (2 + random(4));
//   hspeed = choose(1, -1) * (1 + random(1));
//   siner = 0;
//   sinerfactor = choose(1, -1) * random(1);
static void native_ctFallobj_Create0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!fallobjCache.ready) return;

    float roomW = runner->currentRoom ? (float)runner->currentRoom->width : 320.0f;
    // GML random(x) = x * (rand()/RAND_MAX)
    float rx = (float)rand() / (float)RAND_MAX;
    inst->x = rx * roomW;

    inst->gravity = 0.02f;
    inst->vspeed = 1.0f;
    inst->imageAlpha = 0.5f;

    // choose(1,-1): 50/50
    float sign1 = (rand() & 1) ? 1.0f : -1.0f;
    float rr4 = ((float)rand() / (float)RAND_MAX) * 4.0f;
    float rotspeed = sign1 * (2.0f + rr4);

    float sign2 = (rand() & 1) ? 1.0f : -1.0f;
    float rr1 = ((float)rand() / (float)RAND_MAX);
    inst->hspeed = sign2 * (1.0f + rr1);
    // After setting hspeed manually, recompute speed/direction for consistency
    Instance_computeSpeedFromComponents(inst);

    float sign3 = (rand() & 1) ? 1.0f : -1.0f;
    float rr = ((float)rand() / (float)RAND_MAX);
    float sinerfactor = sign3 * rr;

    Instance_setSelfVar(inst, fallobjCache.siner, RValue_makeReal(0.0));
    Instance_setSelfVar(inst, fallobjCache.sinerfactor, RValue_makeReal((GMLReal)sinerfactor));
    Instance_setSelfVar(inst, fallobjCache.rotspeed, RValue_makeReal((GMLReal)rotspeed));
}

static void native_ctFallobj_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!fallobjCache.ready) return;

    if (inst->y > 250.0f) {
        Runner_destroyInstance(runner, inst);
        return;
    }

    GMLReal siner = RValue_toReal(Instance_getSelfVar(inst, fallobjCache.siner)) + 1.0;
    GMLReal sfactor = RValue_toReal(Instance_getSelfVar(inst, fallobjCache.sinerfactor));
    GMLReal rotspeed = RValue_toReal(Instance_getSelfVar(inst, fallobjCache.rotspeed));

    inst->x += GMLReal_sin(siner / 5.0) * sfactor;
    inst->y += GMLReal_cos(siner / 6.0) * sfactor;
    inst->imageAngle += (float)rotspeed;

    Instance_setSelfVar(inst, fallobjCache.siner, RValue_makeReal(siner));
}

// =====================================================================
// NATIVE: obj_time_Step_1 — THE hottest script in overworld (473 lines!)
// On PSP: skip joystick handling (done in main.c), debug, fullscreen.
// Only keep: time increment, keyboard input mapping, quit handling.
// =====================================================================
static struct {
    int32_t paused, time_var, up, down, left, right;
    int32_t try_up, try_down, try_left, try_right;
    int32_t idle, idle_time, canquit, quit, h_skip;
    int32_t started, debug_r, spec_rtimer;
    int32_t gDebug, gFlag, gOsflavor;
    int32_t scrControlUpdate;
    // Globals for inlined control_update (avoids VM_callCodeIndex every frame)
    int32_t gControlState, gControlNewState, gControlPressed;
    bool ready;
} timeCache = { .ready = false };

static void initTimeCache(VMContext* ctx, DataWin* dw) {
    timeCache.paused = findSelfVarId(dw, "paused");
    timeCache.time_var = findSelfVarId(dw, "time");
    timeCache.up = findSelfVarId(dw, "up");
    timeCache.down = findSelfVarId(dw, "down");
    timeCache.left = findSelfVarId(dw, "left");
    timeCache.right = findSelfVarId(dw, "right");
    timeCache.try_up = findSelfVarId(dw, "try_up");
    timeCache.try_down = findSelfVarId(dw, "try_down");
    timeCache.try_left = findSelfVarId(dw, "try_left");
    timeCache.try_right = findSelfVarId(dw, "try_right");
    timeCache.idle = findSelfVarId(dw, "idle");
    timeCache.idle_time = findSelfVarId(dw, "idle_time");
    timeCache.canquit = findSelfVarId(dw, "canquit");
    timeCache.quit = findSelfVarId(dw, "quit");
    timeCache.h_skip = findSelfVarId(dw, "h_skip");
    timeCache.started = findSelfVarId(dw, "started");
    timeCache.debug_r = findSelfVarId(dw, "debug_r");
    timeCache.spec_rtimer = findSelfVarId(dw, "spec_rtimer");
    timeCache.gDebug = findGlobalVarId(ctx, "debug");
    timeCache.gFlag = findGlobalVarId(ctx, "flag");
    timeCache.gOsflavor = findGlobalVarId(ctx, "osflavor");
    timeCache.scrControlUpdate = findScriptCodeId(ctx, "control_update");
    timeCache.gControlState = findGlobalVarId(ctx, "control_state");
    timeCache.gControlNewState = findGlobalVarId(ctx, "control_new_state");
    timeCache.gControlPressed = findGlobalVarId(ctx, "control_pressed");
    timeCache.ready = (timeCache.up >= 0 && timeCache.started >= 0);
}

static void native_time_Step1(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!timeCache.ready) return;

    // If not started, let VM handle init (first-time config loading)
    GMLReal started = RValue_toReal(Instance_getSelfVar(inst, timeCache.started));
    if (started <= 0) {
        // Fall back to VM for initialization
        int32_t codeId = -1;
        for (uint32_t ci = 0; ci < ctx->dataWin->code.count; ci++) {
            if (strcmp(ctx->dataWin->code.entries[ci].name, "gml_Object_obj_time_Step_1") == 0) {
                codeId = (int32_t)ci; break;
            }
        }
        if (codeId >= 0) {
            RValue r = VM_executeCode(ctx, codeId);
            RValue_free(&r);
        }
        return;
    }

    // time += 1 (if not paused)
    GMLReal paused = RValue_toReal(Instance_getSelfVar(inst, timeCache.paused));
    if (!paused) {
        GMLReal t = RValue_toReal(Instance_getSelfVar(inst, timeCache.time_var));
        Instance_setSelfVar(inst, timeCache.time_var, RValue_makeReal(t + 1.0));
    }

    // Inline control_update (GML script) — direct access to keyboard state +
    // global.control_state[]. Replaces VM_callCodeIndex which dispatches 6+
    // keyboard_check calls + array updates. On PSP saves ~40us/frame.
    {
        RunnerKeyboardState* _kb = runner->keyboard;
        bool s0 = _kb->keyDown[90] || _kb->keyDown[13]; // Z or Enter
        bool s1 = _kb->keyDown[88] || _kb->keyDown[16]; // X or Shift
        bool s2 = _kb->keyDown[67] || _kb->keyDown[17]; // C or Ctrl
        bool newS[3] = { s0, s1, s2 };
        if (timeCache.gControlNewState >= 0 && timeCache.gControlState >= 0 &&
            timeCache.gControlPressed >= 0) {
            for (int i = 0; i < 3; i++) {
                GMLReal prev = getGlobalArray(ctx, timeCache.gControlState, i);
                globalArraySet(ctx, timeCache.gControlNewState, i, RValue_makeReal(newS[i] ? 1.0 : 0.0));
                // control_pressed[i] = !state[i] && new_state[i]
                bool pressed = (prev == 0.0) && newS[i];
                globalArraySet(ctx, timeCache.gControlPressed, i, RValue_makeReal(pressed ? 1.0 : 0.0));
                globalArraySet(ctx, timeCache.gControlState, i, RValue_makeReal(newS[i] ? 1.0 : 0.0));
            }
        }
    }

    // PSP: SKIP joystick handling (lines 104-317 in GML) — done in main.c

    // Keyboard input → self.up/down/left/right. Direct keyboard state reads
    // instead of VMBuiltins_find("keyboard_check") dispatches (~16 calls/frame).
    RunnerKeyboardState* kb = runner->keyboard;
    #define KB(key)  (kb->keyDown[key])
    #define KBR(key) (kb->keyReleased[key])

    // try_* tracking
    if (KB(38)) Instance_setSelfVar(inst, timeCache.try_up, RValue_makeReal(1));
    if (KBR(38)) Instance_setSelfVar(inst, timeCache.try_up, RValue_makeReal(0));
    if (KB(40)) Instance_setSelfVar(inst, timeCache.try_down, RValue_makeReal(1));
    if (KBR(40)) Instance_setSelfVar(inst, timeCache.try_down, RValue_makeReal(0));
    if (KB(39)) Instance_setSelfVar(inst, timeCache.try_right, RValue_makeReal(1));
    if (KBR(39)) Instance_setSelfVar(inst, timeCache.try_right, RValue_makeReal(0));
    if (KB(37)) Instance_setSelfVar(inst, timeCache.try_left, RValue_makeReal(1));
    if (KBR(37)) Instance_setSelfVar(inst, timeCache.try_left, RValue_makeReal(0));

    // up/down/left/right = keyboard_check if try_*
    GMLReal up = 0, down = 0, left = 0, right = 0;
    if (RValue_toReal(Instance_getSelfVar(inst, timeCache.try_up)))    up = KB(38) ? 1 : 0;
    if (RValue_toReal(Instance_getSelfVar(inst, timeCache.try_down)))  down = KB(40) ? 1 : 0;
    if (RValue_toReal(Instance_getSelfVar(inst, timeCache.try_right))) right = KB(39) ? 1 : 0;
    if (RValue_toReal(Instance_getSelfVar(inst, timeCache.try_left)))  left = KB(37) ? 1 : 0;

    if (KBR(38)) up = 0;
    if (KBR(40)) down = 0;
    if (KBR(37)) left = 0;
    if (KBR(39)) right = 0;

    Instance_setSelfVar(inst, timeCache.up, RValue_makeReal(up));
    Instance_setSelfVar(inst, timeCache.down, RValue_makeReal(down));
    Instance_setSelfVar(inst, timeCache.left, RValue_makeReal(left));
    Instance_setSelfVar(inst, timeCache.right, RValue_makeReal(right));

    // idle detection
    // Inline control_check: direct read of global.control_state[0..2]
    bool nowIdle = true;
    if (up || down || left || right) nowIdle = false;
    if (nowIdle && timeCache.gControlState >= 0) {
        for (int b = 0; b < 3; b++) {
            if (getGlobalArray(ctx, timeCache.gControlState, b) != 0) {
                nowIdle = false; break;
            }
        }
    }
    GMLReal wasIdle = RValue_toReal(Instance_getSelfVar(inst, timeCache.idle));
    if (nowIdle && !wasIdle) {
        // current_time is a built-in VARIABLE (not function) — use VMBuiltins_getVariable
        RValue ct = VMBuiltins_getVariable(ctx, BUILTIN_VAR_CURRENT_TIME, "current_time", -1);
        Instance_setSelfVar(inst, timeCache.idle_time, ct);
        RValue_free(&ct);
    }
    Instance_setSelfVar(inst, timeCache.idle, RValue_makeReal(nowIdle ? 1 : 0));

    // Skip: debug (lines 422-432), fullscreen (434-440) — not relevant on PSP

    // Quit handling
    GMLReal canquit = RValue_toReal(Instance_getSelfVar(inst, timeCache.canquit));
    if (canquit == 1.0) {
        GMLReal specRtimer = RValue_toReal(Instance_getSelfVar(inst, timeCache.spec_rtimer)) + 1.0;
        Instance_setSelfVar(inst, timeCache.spec_rtimer, RValue_makeReal(specRtimer));
        if (specRtimer >= 6.0)
            Instance_setSelfVar(inst, timeCache.debug_r, RValue_makeReal(0));

        if (KB(27)) {
            GMLReal q = RValue_toReal(Instance_getSelfVar(inst, timeCache.quit)) + 1.0;
            Instance_setSelfVar(inst, timeCache.quit, RValue_makeReal(q));
            // instance_exists(140) == 0 → instance_create(0,0,140)
            BuiltinFunc instExists = VMBuiltins_find("instance_exists");
            if (instExists) {
                RValue a = RValue_makeReal(140);
                RValue r = instExists(ctx, &a, 1);
                if (RValue_toReal(r) == 0) {
                    Runner_createInstance(runner, 0, 0, 140);
                }
                RValue_free(&r);
            }
        } else {
            Instance_setSelfVar(inst, timeCache.quit, RValue_makeReal(0));
        }
    }

    #undef KB
    #undef KBR
}

// ===[ Initialization ]===

// =====================================================================
// NATIVE: action_kill_object() — just instance_destroy()
// Used by whtpxlgrav (vaporize particles), 93× per frame
// =====================================================================
static void native_actionKillObject(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    Runner_destroyInstance(runner, inst);
}

// =====================================================================
// NATIVE: Generic simple scripts (overworld objects)
// These are trivial scripts that run every frame — avoid VM overhead.
// =====================================================================

// obj_readable_Step_0 / obj_readablesolid_Step_0 (identical logic)
// 4× per frame in ruins3
static struct { int32_t myinteract, mydialoguer; bool ready; } readableCache = {.ready=false};
static void initReadableCache(DataWin* dw) {
    readableCache.myinteract = findSelfVarId(dw, "myinteract");
    readableCache.mydialoguer = findSelfVarId(dw, "mydialoguer");
    readableCache.ready = (readableCache.myinteract >= 0);
}
static void native_readable_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!readableCache.ready) return;
    int32_t mi = selfInt(inst, readableCache.myinteract);
    if (mi == 1) {
        globalSet(ctx, findGlobalVarId(ctx, "interact"), RValue_makeReal(1));
        inst->alarm[0] = 1;
        Instance_setSelfVar(inst, readableCache.myinteract, RValue_makeReal(2));
    } else if (mi == 3) {
        int32_t dlgId = (int32_t)selfReal(inst, readableCache.mydialoguer);
        // instance_exists check
        bool exists = false;
        repeat((int32_t)arrlen(runner->instances), i) {
            if (runner->instances[i]->active && (int32_t)runner->instances[i]->instanceId == dlgId) { exists = true; break; }
        }
        if (!exists) {
            globalSet(ctx, findGlobalVarId(ctx, "interact"), RValue_makeReal(0));
            Instance_setSelfVar(inst, readableCache.myinteract, RValue_makeReal(0));
        }
    }
}

// obj_spikes_room_Step_0: if (global.plot > yarl) image_index = 1; + room checks
static struct { int32_t yarl, plot, flag; bool ready; } spikesCache = {.ready=false};
static void initSpikesCache(VMContext* ctx, DataWin* dw) {
    spikesCache.yarl = findSelfVarId(dw, "yarl");
    spikesCache.plot = findGlobalVarId(ctx, "plot");
    spikesCache.flag = findGlobalVarId(ctx, "flag");
    spikesCache.ready = (spikesCache.yarl >= 0 && spikesCache.plot >= 0);
}
static void native_spikesRoom_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!spikesCache.ready) return;
    GMLReal plot = globalReal(ctx, spikesCache.plot);
    GMLReal yarl = selfReal(inst, spikesCache.yarl);
    if (plot > yarl) inst->imageIndex = 1;
    int32_t roomIdx = runner->currentRoomIndex;
    if (roomIdx == 15 && spikesCache.flag >= 0) {
        if ((int32_t)getGlobalArray(ctx, spikesCache.flag, 35) == 1)
            inst->imageIndex = 1;
    }
    if (roomIdx == 17 && spikesCache.flag >= 0) {
        if ((int32_t)getGlobalArray(ctx, spikesCache.flag, 33) == 1)
            inst->imageIndex = 1;
    }
}

// obj_torbody_Step_0: if (!instance_exists(764)) instance_destroy();
static void native_torbody_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    bool exists = false;
    int32_t count = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < count; i++) {
        if (runner->instances[i]->active && runner->instances[i]->objectIndex == 764) { exists = true; break; }
    }
    if (!exists) Runner_destroyInstance(runner, inst);
}

// obj_plotwall1_Step_0: if (global.plot > 4) instance_destroy();
static void native_plotwall1_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (spikesCache.ready && globalReal(ctx, spikesCache.plot) > 4.0)
        Runner_destroyInstance(runner, inst);
}

// obj_face_torielblink_Step_0: sprite switching based on global.faceemotion
static struct { int32_t faceemotion; bool ready; } faceCache = {.ready=false};
static void initFaceCache(VMContext* ctx) {
    faceCache.faceemotion = findGlobalVarId(ctx, "faceemotion");
    faceCache.ready = (faceCache.faceemotion >= 0);
}
static void native_faceBlink_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)runner;
    if (!faceCache.ready) return;
    int32_t emo = (int32_t)globalReal(ctx, faceCache.faceemotion);
    static const int32_t spriteMap[] = {2108, 2110, 2109, 2102, 2103, 2100, 2095, 2098, 2099, 2090};
    if (emo >= 0 && emo <= 9 && inst->spriteIndex != spriteMap[emo])
        inst->spriteIndex = spriteMap[emo];
}

// obj_base_writer_Step_1: shake/halt logic
static void native_writerStep1(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    GMLReal shake = selfReal(inst, writerCache.shake);
    int32_t halt = selfInt(inst, writerCache.halt);
    int32_t dfy = selfInt(inst, writerCache.dfy);
    if (shake > 38) { inst->speed = 2; inst->direction += 20; }
    else if (shake == 42) { inst->speed = 4; inst->direction -= 19; }
    if (halt == 3 || dfy == 1) Runner_destroyInstance(runner, inst);
}

// obj_base_writer_Step_0: if (control_check_pressed(0) == 1) event_user(0);
// Same pattern as battlecontroller_Step_1 but for writer
static void native_writerStep0(VMContext* ctx, Runner* runner, Instance* inst) {
    BuiltinFunc fn = VMBuiltins_find("control_check_pressed");
    if (!fn) return;
    RValue arg = RValue_makeReal(0);
    RValue result = fn(ctx, &arg, 1);
    if (RValue_toReal(result) == 1.0) {
        Runner_executeEvent(runner, inst, 7, 10); // EVENT_OTHER, OTHER_USER0
    }
    RValue_free(&result);
}

// =====================================================================
// NATIVE: obj_finalbarrier_Draw_0 (100× per frame at barrier scene!)
// GML: script_execute(scr_colorcycle); draw_set_color(color);
//      ossafe_fill_rectangle(x, y, room_width/2 + room_width/m, room_height/2 + room_height/m);
// scr_colorcycle: for i=0..2: bounce c[i] between 10..250, color = make_color_rgb(c[0],c[1],c[2])
// =====================================================================
static struct {
    int32_t c_var, u_var, color_var, m_var;
    bool ready;
} barrierCache = { .ready = false };

static void initBarrierCache(DataWin* dw) {
    barrierCache.c_var = findSelfVarId(dw, "c");
    barrierCache.u_var = findSelfVarId(dw, "u");
    barrierCache.color_var = findSelfVarId(dw, "color");
    barrierCache.m_var = findSelfVarId(dw, "m");
    barrierCache.ready = (barrierCache.c_var >= 0 && barrierCache.u_var >= 0 &&
                          barrierCache.color_var >= 0 && barrierCache.m_var >= 0);
}

static void native_finalbarrier_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!barrierCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    // Inline scr_colorcycle: bounce c[0..2] between 10..250
    for (int i = 0; i < 3; i++) {
        int64_t cKey = ((int64_t)barrierCache.c_var << 32) | (uint32_t)i;
        int64_t uKey = ((int64_t)barrierCache.u_var << 32) | (uint32_t)i;
        ptrdiff_t ci = hmgeti(inst->selfArrayMap, cKey);
        ptrdiff_t ui = hmgeti(inst->selfArrayMap, uKey);
        if (ci < 0 || ui < 0) continue;

        GMLReal c = RValue_toReal(inst->selfArrayMap[ci].value);
        GMLReal u = RValue_toReal(inst->selfArrayMap[ui].value);

        if (c < 10) u = 1;
        if (c > 250) u = 0;
        if (u == 1) c += 2; else c -= 3;

        RValue_free(&inst->selfArrayMap[ci].value);
        inst->selfArrayMap[ci].value = RValue_makeReal(c);
        RValue_free(&inst->selfArrayMap[ui].value);
        inst->selfArrayMap[ui].value = RValue_makeReal(u);
    }

    // color = make_color_rgb(c[0], c[1], c[2])
    GMLReal c0 = 0, c1 = 0, c2 = 0;
    { int64_t k = ((int64_t)barrierCache.c_var << 32) | 0u; ptrdiff_t i = hmgeti(inst->selfArrayMap, k); if (i >= 0) c0 = RValue_toReal(inst->selfArrayMap[i].value); }
    { int64_t k = ((int64_t)barrierCache.c_var << 32) | 1u; ptrdiff_t i = hmgeti(inst->selfArrayMap, k); if (i >= 0) c1 = RValue_toReal(inst->selfArrayMap[i].value); }
    { int64_t k = ((int64_t)barrierCache.c_var << 32) | 2u; ptrdiff_t i = hmgeti(inst->selfArrayMap, k); if (i >= 0) c2 = RValue_toReal(inst->selfArrayMap[i].value); }

    uint32_t color = ((uint32_t)c2 << 16) | ((uint32_t)c1 << 8) | (uint32_t)c0; // BGR
    Instance_setSelfVar(inst, barrierCache.color_var, RValue_makeReal((GMLReal)color));

    // draw_set_color(color); ossafe_fill_rectangle(x, y, rw/2+rw/m, rh/2+rh/m)
    r->drawColor = color;
    GMLReal m = selfReal(inst, barrierCache.m_var);
    if (m == 0) m = 1;
    float rw = (float)runner->currentRoom->width;
    float rh = (float)runner->currentRoom->height;
    float x2 = (rw / 2.0f) + (rw / (float)m);
    float y2 = (rh / 2.0f) + (rh / (float)m);
    r->vtable->drawRectangle(r, inst->x, inst->y, x2, y2, color, r->drawAlpha, false);
}

// =====================================================================
// NATIVE: Asgore battle scripts (240×, 239×, 90×, 22× per frame!)
// =====================================================================

// obj_sinefire_asghelix_Step_0: s+=1; x+=sin(s/sv)*sf; destroy if y>room_h+100; collision check
static struct { int32_t s, sv, sf; bool ready; } asghelixCache = {.ready=false};
static void initAsghelixCache(DataWin* dw) {
    asghelixCache.s = findSelfVarId(dw, "s");
    asghelixCache.sv = findSelfVarId(dw, "sv");
    asghelixCache.sf = findSelfVarId(dw, "sf");
    asghelixCache.ready = (asghelixCache.s >= 0 && asghelixCache.sv >= 0 && asghelixCache.sf >= 0);
}
static void native_asghelix_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!asghelixCache.ready) return;
    GMLReal s = selfReal(inst, asghelixCache.s) + 1.0;
    GMLReal sv = selfReal(inst, asghelixCache.sv);
    GMLReal sf = selfReal(inst, asghelixCache.sf);
    Instance_setSelfVar(inst, asghelixCache.s, RValue_makeReal(s));
    if (sv != 0) inst->x += (float)(GMLReal_sin(s / sv) * sf);
    if (inst->y > (float)(runner->currentRoom->height + 100)) {
        Runner_destroyInstance(runner, inst); return;
    }
    if (checkHeartCollision(runner, inst))
        Runner_executeEvent(runner, inst, 7, 10);
}

// obj_asgorebulparent_Step_2: same as blt_parent_Step_2
// Already have native_bltParent_Step2 — reuse it!

// obj_orangeparticle_Step_0: size lerp + alpha fade + sin/cos movement
static struct { int32_t goalsize, size_var, siner, gg, rr, vv; bool ready; } orangeCache = {.ready=false};
static void initOrangeCache(DataWin* dw) {
    orangeCache.goalsize = findSelfVarId(dw, "goalsize");
    orangeCache.size_var = findSelfVarId(dw, "size");
    orangeCache.siner = findSelfVarId(dw, "siner");
    orangeCache.gg = findSelfVarId(dw, "gg");
    orangeCache.rr = findSelfVarId(dw, "rr");
    orangeCache.vv = findSelfVarId(dw, "vv");
    orangeCache.ready = (orangeCache.goalsize >= 0 && orangeCache.size_var >= 0 &&
                         orangeCache.siner >= 0 && orangeCache.gg >= 0);
}
static void native_orangeparticle_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!orangeCache.ready) return;
    GMLReal goalsize = selfReal(inst, orangeCache.goalsize);
    GMLReal size = selfReal(inst, orangeCache.size_var);
    if (goalsize > size + 0.1) size += 0.03;
    if (goalsize < size - 0.1) size -= 0.03;
    inst->imageXscale = (float)size;
    inst->imageYscale = (float)size;
    inst->imageAlpha -= 0.01f;
    if (inst->imageAlpha < 0.01f) { Runner_destroyInstance(runner, inst); return; }
    GMLReal siner = selfReal(inst, orangeCache.siner) + 1.0;
    GMLReal gg = selfReal(inst, orangeCache.gg);
    GMLReal rr = selfReal(inst, orangeCache.rr);
    GMLReal vv = selfReal(inst, orangeCache.vv);
    if (gg != 0) {
        inst->x += (float)(GMLReal_sin(siner / gg) * rr);
        inst->y += (float)(GMLReal_cos(siner / gg) * vv);
    }
    Instance_setSelfVar(inst, orangeCache.size_var, RValue_makeReal(size));
    Instance_setSelfVar(inst, orangeCache.siner, RValue_makeReal(siner));
}

// obj_mercybutton_part_Step_0: image_angle += aa;
static struct { int32_t aa; bool ready; } mercypartCache = {.ready=false};
static void initMercypartCache(DataWin* dw) {
    mercypartCache.aa = findSelfVarId(dw, "aa");
    mercypartCache.ready = (mercypartCache.aa >= 0);
}
static void native_mercypart_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    if (!mercypartCache.ready) return;
    inst->imageAngle += (float)selfReal(inst, mercypartCache.aa);
}

// =====================================================================
// NATIVE: obj_purplegradienter_Draw_0 (239us on PC! 10 rects per frame)
// =====================================================================
static struct { int32_t pg_siner, pg_amt, pg_fade; bool ready; } purplegradCache = {.ready=false};
static void initPurplegradCache(DataWin* dw) {
    purplegradCache.pg_siner = findSelfVarId(dw, "siner");
    purplegradCache.pg_amt = findSelfVarId(dw, "amt");
    purplegradCache.pg_fade = findSelfVarId(dw, "fade");
    purplegradCache.ready = (purplegradCache.pg_siner >= 0 && purplegradCache.pg_amt >= 0);
}
static void native_purplegradienter_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!purplegradCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    GMLReal siner = selfReal(inst, purplegradCache.pg_siner);
    GMLReal amt = selfReal(inst, purplegradCache.pg_amt);
    GMLReal ac = 1.5 + GMLReal_sin(siner / 20.0);
    Instance_setSelfVar(inst, purplegradCache.pg_siner, RValue_makeReal(siner + 1.0));

    float rw = (float)runner->currentRoom->width;
    float rh = (float)runner->currentRoom->height;

    r->drawAlpha = 1.0f;
    for (int i = 0; i < 10; i++) {
        float a = (float)((0.8 - ((float)i / 16.0)) * amt);
        if (a < 0) a = 0; if (a > 1) a = 1;
        float y1 = rh - (float)(i * i * ac);
        float y2 = rh - (float)((i + 1) * (i + 1) * ac);
        r->vtable->drawRectangle(r, -10.0f, y1, rw + 10.0f, y2, 8388736, a, false);
    }
    r->drawAlpha = 1.0f;

    if (purplegradCache.pg_fade >= 0) {
        GMLReal fade = selfReal(inst, purplegradCache.pg_fade);
        if (fade == 1.0) {
            amt -= 0.03;
            Instance_setSelfVar(inst, purplegradCache.pg_amt, RValue_makeReal(amt));
            if (amt < 0.05) Runner_destroyInstance(runner, inst);
        }
    }
}

// =====================================================================
// NATIVE: PSP no-ops / simplified for PSP-irrelevant Draw events
// These run every frame in every room but are PC/console-specific
// =====================================================================
static void native_noop(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner; (void)inst;
}

// =====================================================================
// NATIVE: obj_asgoreb_body_Draw_0 (Asgore body — 8 draw_sprite_ext + sin/cos animation)
// 168us on PC = ~1.2ms on PSP — biggest single Draw script!
// =====================================================================
static struct {
    int32_t part, partx, party, siner, fakeanim, moving;
    bool ready;
} asgBodyCache = { .ready = false };

static void initAsgBodyCache(DataWin* dw) {
    asgBodyCache.part = findSelfVarId(dw, "part");
    asgBodyCache.partx = findSelfVarId(dw, "partx");
    asgBodyCache.party = findSelfVarId(dw, "party");
    asgBodyCache.siner = findSelfVarId(dw, "siner");
    asgBodyCache.fakeanim = findSelfVarId(dw, "fakeanim");
    asgBodyCache.moving = findSelfVarId(dw, "moving");
    asgBodyCache.ready = (asgBodyCache.part >= 0 && asgBodyCache.partx >= 0 &&
                          asgBodyCache.party >= 0 && asgBodyCache.siner >= 0 &&
                          asgBodyCache.fakeanim >= 0 && asgBodyCache.moving >= 0);
}

// =====================================================================
// NATIVE: obj_itemswapper_Draw_0 (menu UI with 18+ draw_text calls = ~2.7ms PSP)
// Native version: direct renderer.drawText calls, cached global arrays, skip VM dispatch.
// Complex helpers (scr_itemname, scr_storagename, scr_drawtext_icons, scr_drawtext_centered)
// are invoked via VM_callCodeIndex — they're called once, not per-character.

static struct {
    int32_t buffer, boxno, boxtype, column, c0y, c1y, spec, noroom;
    int32_t gInteract, gLanguage, gItemname, gItem, gFlag, gMenuno;
    int32_t objOverworldctrl;
    int32_t scrItemname, scrStoragename, scrDrawtextCentered, scrDrawtextIcons,
            scrStorageget, scrItemshift, scrItemget, scrStorageshift;
    int32_t scrControlCheckPressed;
    bool ready;
} swapperCache = { .ready = false };

static void initSwapperCache(VMContext* ctx, DataWin* dw) {
    swapperCache.buffer = findSelfVarId(dw, "buffer");
    swapperCache.boxno = findSelfVarId(dw, "boxno");
    swapperCache.boxtype = findSelfVarId(dw, "boxtype");
    swapperCache.column = findSelfVarId(dw, "column");
    swapperCache.c0y = findSelfVarId(dw, "c0y");
    swapperCache.c1y = findSelfVarId(dw, "c1y");
    swapperCache.spec = findSelfVarId(dw, "spec");
    swapperCache.noroom = findSelfVarId(dw, "noroom");
    swapperCache.gInteract = findGlobalVarId(ctx, "interact");
    swapperCache.gLanguage = findGlobalVarId(ctx, "language");
    swapperCache.gItemname = findGlobalVarId(ctx, "itemname");
    swapperCache.gItem = findGlobalVarId(ctx, "item");
    swapperCache.gFlag = findGlobalVarId(ctx, "flag");
    swapperCache.gMenuno = findGlobalVarId(ctx, "menuno");
    swapperCache.objOverworldctrl = findObjectIndex(dw, "obj_overworldcontroller");
    swapperCache.scrItemname = findScriptCodeId(ctx, "scr_itemname");
    swapperCache.scrStoragename = findScriptCodeId(ctx, "scr_storagename");
    swapperCache.scrDrawtextCentered = findScriptCodeId(ctx, "scr_drawtext_centered");
    swapperCache.scrDrawtextIcons = findScriptCodeId(ctx, "scr_drawtext_icons");
    swapperCache.scrStorageget = findScriptCodeId(ctx, "scr_storageget");
    swapperCache.scrItemshift = findScriptCodeId(ctx, "scr_itemshift");
    swapperCache.scrItemget = findScriptCodeId(ctx, "scr_itemget");
    swapperCache.scrStorageshift = findScriptCodeId(ctx, "scr_storageshift");
    swapperCache.scrControlCheckPressed = findScriptCodeId(ctx, "control_check_pressed");
    swapperCache.ready = (swapperCache.buffer >= 0 && swapperCache.gItemname >= 0);
}


static inline void swapperDrawText(Runner* runner, float x, float y, const char* text) {
    Renderer* r = runner->renderer;
    if (!r || !text) return;
    char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, text);
    r->vtable->drawText(r, processed, x, y, 1.0f, 1.0f, 0.0f);
    free(processed);
}


static const char* swapperGetArrayString(VMContext* ctx, int32_t varID, int32_t idx) {
    int64_t k = ((int64_t)varID << 32) | (uint32_t)idx;
    ptrdiff_t i = hmgeti(ctx->globalArrayMap, k);
    if (i < 0) return "";
    RValue v = ctx->globalArrayMap[i].value;
    if (v.type == RVALUE_STRING && v.string) return v.string;
    return "";
}

static void native_itemswapper_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!swapperCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    
    GMLReal buffer = selfReal(inst, swapperCache.buffer) + 1.0;
    Instance_setSelfVar(inst, swapperCache.buffer, RValue_makeReal(buffer));

    
    if (swapperCache.gInteract >= 0) {
        RValue_free(&ctx->globalVars[swapperCache.gInteract]);
        ctx->globalVars[swapperCache.gInteract] = RValue_makeReal(1.0);
    }

    if (buffer <= 3) return;

    int32_t boxtype = selfInt(inst, swapperCache.boxtype);
    int32_t boxno = (boxtype == 1) ? 312 : 300;
    Instance_setSelfVar(inst, swapperCache.boxno, RValue_makeReal((GMLReal)boxno));

    
    float xx = (float)runner->currentRoom->views[runner->viewCurrent].viewX;
    float yy = (float)runner->currentRoom->views[runner->viewCurrent].viewY + 6.0f;

    
    const char* lang = globalString(ctx, swapperCache.gLanguage);
    bool isJa = (lang && strcmp(lang, "ja") == 0);
    float boxofs = isJa ? 6.0f : 8.0f;
    float heartofs = isJa ? 7.0f : 9.0f;
    float itemofs = isJa ? 19.0f : 23.0f;

    
    r->drawColor = 0xFFFFFF;
    {
        float x1 = xx + boxofs, y1 = yy + (boxofs - 6.0f);
        float x2 = xx + (320.0f - boxofs), y2 = yy + (234.0f - boxofs);
        r->vtable->drawRectangle(r, x1, y1, x2, y2, 0xFFFFFF, r->drawAlpha, false);
    }
    
    r->drawColor = 0;
    {
        float x1 = xx + (boxofs + 3.0f), y1 = yy + (boxofs - 3.0f);
        float x2 = xx + (317.0f - boxofs), y2 = yy + (231.0f - boxofs);
        r->vtable->drawRectangle(r, x1, y1, x2, y2, 0, r->drawAlpha, false);
    }

    
    if (!isJa) r->drawFont = 2;
    else r->drawFont = 14; 

    r->drawColor = 0xFFFFFF;
    r->drawHalign = 0;
    r->drawValign = 0;

    
    
    
    {
        BuiltinFunc gt = VMBuiltins_find("scr_gettext");
        if (gt) {
            for (int32_t ii = 0; ii < 8; ii++) {
                int32_t itemid = (int32_t)getGlobalArray(ctx, swapperCache.gItem, ii);
                char key[32];
                snprintf(key, sizeof(key), "item_name_%d", itemid);
                RValue arg = RValue_makeString(key);
                RValue result = gt(ctx, &arg, 1);
                globalArraySet(ctx, swapperCache.gItemname, ii, result);
            }
        }
    }

    
    for (int i = 0; i < 8; i++) {
        r->drawColor = 0xFFFFFF;
        const char* name = swapperGetArrayString(ctx, swapperCache.gItemname, i);
        swapperDrawText(runner, xx + boxofs + 3.0f + itemofs, yy + 30.0f + (float)(i * 16), name);

        GMLReal itemVal = (swapperCache.gItem >= 0) ? getGlobalArray(ctx, swapperCache.gItem, i) : 0;
        if ((int32_t)itemVal == 0) {
            r->drawColor = 0xFF; 
            r->vtable->drawLine(r, xx + boxofs + 3.0f + itemofs + 5.0f, yy + 40.0f + (float)(i * 16),
                                   xx + boxofs + 3.0f + itemofs + 95.0f, yy + 40.0f + (float)(i * 16),
                                   1.0f, 0xFF, r->drawAlpha);
        }
    }

    r->drawColor = 0xFFFFFF;

    
    {
        BuiltinFunc getText = VMBuiltins_find("scr_gettext");
        BuiltinFunc stringWidth = VMBuiltins_find("string_width");
        
        if (getText && stringWidth) {
            RValue gtArg = RValue_makeString("itembox_title_inventory");
            RValue s = getText(ctx, &gtArg, 1);
            const char* text = (s.type == RVALUE_STRING && s.string) ? s.string : "";
            RValue wr = stringWidth(ctx, &s, 1);
            float w = (float)RValue_toReal(wr);
            RValue_free(&wr);
            float cx = xx + boxofs + 3.0f + itemofs + 50.0f;
            swapperDrawText(runner, cx - w / 2.0f, yy + 9.0f, text);
            RValue_free(&s);
        }
        
        if (getText && stringWidth) {
            RValue gtArg = RValue_makeString("itembox_title_box");
            RValue s = getText(ctx, &gtArg, 1);
            const char* text = (s.type == RVALUE_STRING && s.string) ? s.string : "";
            RValue wr = stringWidth(ctx, &s, 1);
            float w = (float)RValue_toReal(wr);
            RValue_free(&wr);
            float cx = xx + 162.0f + itemofs + 50.0f;
            swapperDrawText(runner, cx - w / 2.0f, yy + 9.0f, text);
            RValue_free(&s);
        }
    }

    
    
    {
        for (int32_t ii = 0; ii <= 10; ii++) {
            globalArraySet(ctx, swapperCache.gItemname, ii, RValue_makeString(" "));
        }
        BuiltinFunc gt = VMBuiltins_find("scr_gettext");
        if (gt && swapperCache.gFlag >= 0) {
            for (int32_t ii = 0; ii < 11; ii++) {
                int32_t itemid = (int32_t)getGlobalArray(ctx, swapperCache.gFlag, boxno + ii);
                char key[32];
                snprintf(key, sizeof(key), "item_name_%d", itemid);
                RValue arg = RValue_makeString(key);
                RValue result = gt(ctx, &arg, 1);
                globalArraySet(ctx, swapperCache.gItemname, ii, result);
            }
        }
    }

    
    for (int i = 0; i < 10; i++) {
        r->drawColor = 0xFFFFFF;
        const char* name = swapperGetArrayString(ctx, swapperCache.gItemname, i);
        swapperDrawText(runner, xx + 162.0f + itemofs, yy + 30.0f + (float)(i * 16), name);

        GMLReal flagVal = (swapperCache.gFlag >= 0) ? getGlobalArray(ctx, swapperCache.gFlag, boxno + i) : 0;
        if ((int32_t)flagVal == 0) {
            r->drawColor = 0xFF;
            r->vtable->drawLine(r, xx + 162.0f + itemofs + 5.0f, yy + 40.0f + (float)(i * 16),
                                   xx + 162.0f + itemofs + 95.0f, yy + 40.0f + (float)(i * 16),
                                   1.0f, 0xFF, r->drawAlpha);
        }
    }

    r->drawColor = 0xFFFFFF;
    r->vtable->drawLine(r, xx + 160.0f, yy + 40.0f, xx + 160.0f, yy + 190.0f, 1.0f, 0xFFFFFF, r->drawAlpha);
    r->vtable->drawLine(r, xx + 161.0f, yy + 40.0f, xx + 161.0f, yy + 190.0f, 1.0f, 0xFFFFFF, r->drawAlpha);

    
    if (swapperCache.scrDrawtextIcons >= 0) {
        RValue args[3];
        args[0] = RValue_makeReal(xx + 100.0f);
        args[1] = RValue_makeReal(yy + 197.0f);
        BuiltinFunc getText = VMBuiltins_find("scr_gettext");
        RValue gtArg = RValue_makeString("itembox_close");
        args[2] = getText ? getText(ctx, &gtArg, 1) : RValue_makeString("");
        RValue res = VM_callCodeIndex(ctx, swapperCache.scrDrawtextIcons, args, 3);
        RValue_free(&res);
        RValue_free(&args[2]);
    }

    
    int32_t column = selfInt(inst, swapperCache.column);
    int32_t c0y = selfInt(inst, swapperCache.c0y);
    int32_t c1y = selfInt(inst, swapperCache.c1y);
    BuiltinFunc kbPressed = VMBuiltins_find("keyboard_check_pressed");
    if (kbPressed) {
        RValue k;
        k = RValue_makeReal(39); 
        if (RValue_toReal(kbPressed(ctx, &k, 1)) > 0 && column != 1) { column = 1; c1y = c0y; }
        k = RValue_makeReal(37); 
        if (RValue_toReal(kbPressed(ctx, &k, 1)) > 0 && column != 0) {
            column = 0; c0y = c1y; if (c0y > 7) c0y = 7;
        }
        k = RValue_makeReal(38); 
        if (RValue_toReal(kbPressed(ctx, &k, 1)) > 0) {
            if (column == 0 && c0y > 0) c0y--;
            if (column == 1 && c1y > 0) c1y--;
        }
        k = RValue_makeReal(40); 
        if (RValue_toReal(kbPressed(ctx, &k, 1)) > 0) {
            if (column == 0 && c0y < 7) c0y++;
            if (column == 1 && c1y < 9) c1y++;
        }
    }
    Instance_setSelfVar(inst, swapperCache.column, RValue_makeReal((GMLReal)column));
    Instance_setSelfVar(inst, swapperCache.c0y, RValue_makeReal((GMLReal)c0y));
    Instance_setSelfVar(inst, swapperCache.c1y, RValue_makeReal((GMLReal)c1y));

    
    if (column == 0)
        Renderer_drawSprite(r, 61, 0, xx + boxofs + 3.0f + heartofs, yy + 35.0f + (float)(16 * c0y));
    else if (column == 1)
        Renderer_drawSprite(r, 61, 0, xx + 162.0f + heartofs, yy + 35.0f + (float)(16 * c1y));

    
    
    {
        int32_t gCP = findGlobalVarId(ctx, "control_pressed");
        bool pressed0 = false;
        if (gCP >= 0) {
            int64_t k = ((int64_t)gCP << 32) | 0u;
            ptrdiff_t idx0 = hmgeti(ctx->globalArrayMap, k);
            pressed0 = (idx0 >= 0) && (RValue_toReal(ctx->globalArrayMap[idx0].value) != 0);
        }
        if (pressed0 && buffer > 6) {
            if (column == 0 && swapperCache.scrStorageget >= 0) {
                GMLReal itemAtC0 = getGlobalArray(ctx, swapperCache.gItem, c0y);
                RValue args[2] = { RValue_makeReal(itemAtC0), RValue_makeReal((GMLReal)boxno) };
                RValue res = VM_callCodeIndex(ctx, swapperCache.scrStorageget, args, 2);
                RValue_free(&res);
                int32_t noroom = selfInt(inst, swapperCache.noroom);
                if (noroom == 0 && swapperCache.scrItemshift >= 0) {
                    RValue shift[2] = { RValue_makeReal((GMLReal)c0y), RValue_makeReal(0) };
                    res = VM_callCodeIndex(ctx, swapperCache.scrItemshift, shift, 2);
                    RValue_free(&res);
                }
            } else if (column == 1 && swapperCache.scrItemget >= 0) {
                GMLReal flagAtC1 = getGlobalArray(ctx, swapperCache.gFlag, c1y + boxno);
                RValue arg = RValue_makeReal(flagAtC1);
                RValue res = VM_callCodeIndex(ctx, swapperCache.scrItemget, &arg, 1);
                RValue_free(&res);
                int32_t noroom = selfInt(inst, swapperCache.noroom);
                if (noroom == 0 && swapperCache.scrStorageshift >= 0) {
                    RValue shift[3] = { RValue_makeReal((GMLReal)c1y), RValue_makeReal(0), RValue_makeReal((GMLReal)boxno) };
                    res = VM_callCodeIndex(ctx, swapperCache.scrStorageshift, shift, 3);
                    RValue_free(&res);
                }
            }
        }
        
        bool pressed1 = false;
        if (gCP >= 0) {
            int64_t k1 = ((int64_t)gCP << 32) | 1u;
            ptrdiff_t idx1 = hmgeti(ctx->globalArrayMap, k1);
            pressed1 = (idx1 >= 0) && (RValue_toReal(ctx->globalArrayMap[idx1].value) != 0);
        }
        if (pressed1) {
            if (swapperCache.gInteract >= 0) {
                RValue_free(&ctx->globalVars[swapperCache.gInteract]);
                ctx->globalVars[swapperCache.gInteract] = RValue_makeReal(0);
            }
            int32_t spec = selfInt(inst, swapperCache.spec);
            if (spec == 1 && swapperCache.gMenuno >= 0) {
                RValue_free(&ctx->globalVars[swapperCache.gMenuno]);
                ctx->globalVars[swapperCache.gMenuno] = RValue_makeReal(0);
            }
            
            if (swapperCache.objOverworldctrl >= 0) {
                Instance* owc = findInstanceByObject(runner, swapperCache.objOverworldctrl);
                if (owc) {
                    int32_t owcBufId = findSelfVarId(ctx->dataWin, "buffer");
                    if (owcBufId >= 0)
                        Instance_setSelfVar(owc, owcBufId, RValue_makeReal(-2.0));
                }
            }
            Runner_destroyInstance(runner, inst);
        }
    }
}







static struct { int32_t rotdir; bool ready; } clawCache = {.ready=false};
static void initClawCache(DataWin* dw) {
    clawCache.rotdir = findSelfVarId(dw, "rotdir");
    clawCache.ready = (clawCache.rotdir >= 0);
}
static void native_clawbullet_white_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!clawCache.ready || !chasefire2Cache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    GMLReal rotdir = selfReal(inst, clawCache.rotdir);

    
    inst->imageAngle = (float)(inst->direction + rotdir * 2.0);

    
    Renderer_drawSpriteExt(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, 1.0f, 1.0f, inst->imageAngle,
                           0xFFFFFF, inst->imageAlpha);

    
    setDirection(inst, inst->direction + rotdir);

    
    updateBorderPosCache(ctx, runner);
    float border12 = 12.0f;
    if (inst->hspeed < 0 && inst->x < (borderPosCache.lbx - border12)) { Runner_destroyInstance(runner, inst); return; }
    if (inst->hspeed > 0 && inst->x > (borderPosCache.rbx + border12)) { Runner_destroyInstance(runner, inst); return; }
    if (inst->vspeed < 0 && inst->y < (borderPosCache.border2 - border12)) { Runner_destroyInstance(runner, inst); return; }
    if (inst->vspeed > 0 && inst->y > (borderPosCache.border3 + border12)) { Runner_destroyInstance(runner, inst); return; }

    
    inst->imageAlpha += 0.1f;
}






static struct {
    int32_t toothxx, toothyy, toothspeed, toothdist, seed, factor;
    bool ready;
} iceteethCache = { .ready = false };

static void initIceteethCache(DataWin* dw) {
    iceteethCache.toothxx = findSelfVarId(dw, "toothxx");
    iceteethCache.toothyy = findSelfVarId(dw, "toothyy");
    iceteethCache.toothspeed = findSelfVarId(dw, "toothspeed");
    iceteethCache.toothdist = findSelfVarId(dw, "toothdist");
    iceteethCache.seed = findSelfVarId(dw, "seed");
    iceteethCache.factor = findSelfVarId(dw, "factor");
    iceteethCache.ready = (iceteethCache.toothxx >= 0 && iceteethCache.toothyy >= 0 &&
                           iceteethCache.toothspeed >= 0 && iceteethCache.toothdist >= 0);
}

static void native_iceteeth_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!iceteethCache.ready || !chasefire2Cache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    
    r->drawColor = 0xFFFFFF;

    
    updateBorderPosCache(ctx, runner);
    GMLReal border0 = borderPosCache.lbx;
    GMLReal border1 = borderPosCache.rbx;
    GMLReal border2 = borderPosCache.border2;
    GMLReal border3 = borderPosCache.border3;

    
    GMLReal seed = selfReal(inst, iceteethCache.seed);
    GMLReal factor = selfReal(inst, iceteethCache.factor);
    GMLReal toothspeed = selfReal(inst, iceteethCache.toothspeed);
    GMLReal toothdist = selfReal(inst, iceteethCache.toothdist);

    
    updateHeartCache(runner);
    bool heartValid = heartCache.inst && heartCache.bbox.valid;
    InstanceBBox hb = heartCache.bbox;

    bool anyCollision = false;

    
    for (int32_t i = 0; (border0 + (i * 5)) < border1; i++) {
        
        GMLReal toothyy_i = inst->y + GMLReal_sin(seed + i * factor) * 30.0;
        selfArraySet(inst, iceteethCache.toothyy, i, RValue_makeReal(toothyy_i));

        
        GMLReal toothxx_i = RValue_toReal(selfArrayGet(inst, iceteethCache.toothxx, i)) + toothspeed;
        if (toothxx_i > border1) toothxx_i = border0;
        if (toothxx_i < border0) toothxx_i = border0;
        selfArraySet(inst, iceteethCache.toothxx, i, RValue_makeReal(toothxx_i));

        float tx = (float)toothxx_i;

        
        if (toothyy_i > border2) {
            r->vtable->drawLine(r, tx, (float)border2, tx, (float)toothyy_i, 1.0f, 0xFFFFFF, r->drawAlpha);
        }

        
        float lowerY = (float)(toothyy_i + toothdist);
        if ((toothyy_i + toothdist) < border3) {
            r->vtable->drawLine(r, tx, (float)border3, tx, lowerY, 1.0f, 0xFFFFFF, r->drawAlpha);
        }

        
        
        
        if (heartValid && !anyCollision) {
            
            float upperY1 = (float)border2, upperY2 = (float)(toothyy_i - 3.0);
            if (upperY1 > upperY2) { float t = upperY1; upperY1 = upperY2; upperY2 = t; }
            if (tx >= hb.left && tx <= hb.right &&
                upperY2 >= hb.top && upperY1 <= hb.bottom) {
                anyCollision = true;
            }
            
            float lowerY1 = (float)border3, lowerY2 = (float)(toothyy_i + toothdist + 3.0);
            if (lowerY1 > lowerY2) { float t = lowerY1; lowerY1 = lowerY2; lowerY2 = t; }
            if (!anyCollision && tx >= hb.left && tx <= hb.right &&
                lowerY2 >= hb.top && lowerY1 <= hb.bottom) {
                anyCollision = true;
            }
        }
    }

    
    if (anyCollision) {
        Runner_executeEvent(runner, inst, 7, 11); 
    }

    
    toothdist -= inst->vspeed * 2.15;
    Instance_setSelfVar(inst, iceteethCache.toothdist, RValue_makeReal(toothdist));

    
    if (toothspeed < 2.4) {
        toothspeed += 0.08;
        Instance_setSelfVar(inst, iceteethCache.toothspeed, RValue_makeReal(toothspeed));
    }
}








static void native_4sidebullet_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!clawCache.ready || !chasefire2Cache.ready) return;

    
    native_drawSelfBorder(ctx, runner, inst);

    GMLReal rotdir = selfReal(inst, clawCache.rotdir);

    
    setDirection(inst, inst->direction + rotdir);

    
    updateBorderPosCache(ctx, runner);
    float b12 = 12.0f;
    if (inst->hspeed < 0 && inst->x < (borderPosCache.lbx - b12)) { Runner_destroyInstance(runner, inst); return; }
    if (inst->hspeed > 0 && inst->x > (borderPosCache.rbx + b12)) { Runner_destroyInstance(runner, inst); return; }
    if (inst->vspeed < 0 && inst->y < (borderPosCache.border2 - b12)) { Runner_destroyInstance(runner, inst); return; }
    if (inst->vspeed > 0 && inst->y > (borderPosCache.border3 + b12)) { Runner_destroyInstance(runner, inst); return; }

    
    inst->imageAlpha += 0.1f;
}








static struct { int32_t objDrakehead; bool ready; } drakebodyCache = {.ready=false};
static void initDrakebodyCache(DataWin* dw) {
    drakebodyCache.objDrakehead = findObjectIndex(dw, "obj_drakehead");
    drakebodyCache.ready = true; 
}
static void native_drakebody_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    Renderer* r = runner->renderer;
    if (!r) return;

    
    Renderer_drawSpriteExt(r, 227, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFF, 1.0f);

    
    Instance* head = (drakebodyCache.objDrakehead >= 0)
                     ? findInstanceByObject(runner, drakebodyCache.objDrakehead) : NULL;

    float yOff = 0;
    if (head) {
        yOff = (head->y - inst->ystart) / 3.0f;
    }
    
    Renderer_drawSpriteExt(r, 226, 0, inst->x, inst->y + yOff, 2.0f, 2.0f, 0.0f, 0xFFFFFF, 1.0f);
}

static void native_asgoreb_body_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!asgBodyCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    GMLReal fakeanim = selfReal(inst, asgBodyCache.fakeanim);
    int32_t faIdx = (int32_t)fakeanim;

    
    for (int i = 0; i < 8; i++) {
        GMLReal partSprite = RValue_toReal(selfArrayGet(inst, asgBodyCache.part, i));
        GMLReal px = RValue_toReal(selfArrayGet(inst, asgBodyCache.partx, i));
        GMLReal py = RValue_toReal(selfArrayGet(inst, asgBodyCache.party, i));
        Renderer_drawSpriteExt(r, (int32_t)partSprite, faIdx,
                               inst->x + (float)px, inst->y + (float)py,
                               2.0f, 2.0f, 0.0f, 0xFFFFFF, 1.0f);
    }

    
    GMLReal siner = selfReal(inst, asgBodyCache.siner) + 1.0;
    fakeanim += 0.1;
    Instance_setSelfVar(inst, asgBodyCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, asgBodyCache.fakeanim, RValue_makeReal(fakeanim));

    
    GMLReal moving = selfReal(inst, asgBodyCache.moving);
    if ((int32_t)moving == 1) {
        GMLReal sinVal = GMLReal_sin(siner / 15.0);
        GMLReal cosVal = GMLReal_cos(siner / 15.0);

        
        RValue p7 = selfArrayGet(inst, asgBodyCache.party, 7);
        selfArraySet(inst, asgBodyCache.party, 7, RValue_makeReal(RValue_toReal(p7) + sinVal * 0.3));
        
        RValue p6 = selfArrayGet(inst, asgBodyCache.party, 6);
        selfArraySet(inst, asgBodyCache.party, 6, RValue_makeReal(RValue_toReal(p6) + sinVal * 0.2));
        
        RValue p5 = selfArrayGet(inst, asgBodyCache.party, 5);
        selfArraySet(inst, asgBodyCache.party, 5, RValue_makeReal(RValue_toReal(p5) + cosVal * 0.1));
        
        RValue p4 = selfArrayGet(inst, asgBodyCache.party, 4);
        selfArraySet(inst, asgBodyCache.party, 4, RValue_makeReal(RValue_toReal(p4) + cosVal * 0.1));
        
        RValue p3 = selfArrayGet(inst, asgBodyCache.party, 3);
        selfArraySet(inst, asgBodyCache.party, 3, RValue_makeReal(RValue_toReal(p3) + sinVal * 0.1));
        
        RValue p0 = selfArrayGet(inst, asgBodyCache.party, 0);
        selfArraySet(inst, asgBodyCache.party, 0, RValue_makeReal(RValue_toReal(p0) + sinVal * 0.05));
    }

    
    selfArraySet(inst, asgBodyCache.part, 7, RValue_makeReal(636));
    selfArraySet(inst, asgBodyCache.part, 6, RValue_makeReal(637));
    selfArraySet(inst, asgBodyCache.part, 5, RValue_makeReal(634));
    selfArraySet(inst, asgBodyCache.part, 4, RValue_makeReal(635));
    selfArraySet(inst, asgBodyCache.part, 3, RValue_makeReal(638));
    selfArraySet(inst, asgBodyCache.part, 2, RValue_makeReal(639));
    selfArraySet(inst, asgBodyCache.part, 1, RValue_makeReal(640));
    selfArraySet(inst, asgBodyCache.part, 0, RValue_makeReal(632));
}





static struct { int32_t objParticle; bool ready; } partgenCache = { .ready = false };
static void initPartgenCache(DataWin* dw) {
    partgenCache.objParticle = findObjectIndex(dw, "obj_orangeparticle");
    partgenCache.ready = (partgenCache.objParticle >= 0);
}
static void native_orangeparticlegen_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!partgenCache.ready) return;
    float rw = (float)runner->currentRoom->width;
    float rh = (float)runner->currentRoom->height;
    
    RValue rndArg = RValue_makeReal((GMLReal)(rw + 200.0));
    RValue rndResult = callBuiltin(ctx, "random", &rndArg, 1);
    GMLReal rx = -100.0 + RValue_toReal(rndResult);
    GMLReal ry = (GMLReal)(rh + 10);
    
    RValue icArgs[3] = { RValue_makeReal(rx), RValue_makeReal(ry), RValue_makeReal((GMLReal)partgenCache.objParticle) };
    RValue icResult = callBuiltin(ctx, "instance_create", icArgs, 3);
    RValue_free(&icResult);
}






static struct { int32_t r, ang, rspeed, angspeed, centerx, centery; bool ready; } cfireCache = {.ready=false};
static void initCfireCache(DataWin* dw) {
    cfireCache.r = findSelfVarId(dw, "r");
    cfireCache.ang = findSelfVarId(dw, "ang");
    cfireCache.rspeed = findSelfVarId(dw, "rspeed");
    cfireCache.angspeed = findSelfVarId(dw, "angspeed");
    cfireCache.centerx = findSelfVarId(dw, "centerx");
    cfireCache.centery = findSelfVarId(dw, "centery");
    cfireCache.ready = (cfireCache.r >= 0 && cfireCache.ang >= 0 && cfireCache.rspeed >= 0 &&
                        cfireCache.angspeed >= 0 && cfireCache.centerx >= 0 && cfireCache.centery >= 0);
}
static void native_cfire_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!cfireCache.ready) return;
    GMLReal r = selfReal(inst, cfireCache.r) - selfReal(inst, cfireCache.rspeed);
    GMLReal ang = selfReal(inst, cfireCache.ang) + selfReal(inst, cfireCache.angspeed);
    Instance_setSelfVar(inst, cfireCache.r, RValue_makeReal(r));
    Instance_setSelfVar(inst, cfireCache.ang, RValue_makeReal(ang));
    if (r <= 0.5) { Runner_destroyInstance(runner, inst); return; }
    GMLReal cx = selfReal(inst, cfireCache.centerx);
    GMLReal cy = selfReal(inst, cfireCache.centery);
    GMLReal angRad = ang * (M_PI / 180.0);
    inst->x = (float)(cx + GMLReal_cos(angRad) * r);
    inst->y = (float)(cy + (-GMLReal_sin(angRad)) * r);
    if (checkHeartCollision(runner, inst))
        Runner_executeEvent(runner, inst, 7, 10);
}





static struct { int32_t negaspeed; bool ready; } genfireCache = {.ready=false};
static void initGenfireCache(DataWin* dw) {
    genfireCache.negaspeed = findSelfVarId(dw, "negaspeed");
    genfireCache.ready = (genfireCache.negaspeed >= 0);
}
static void native_genericfire_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!genfireCache.ready) return;
    if (checkHeartCollision(runner, inst))
        Runner_executeEvent(runner, inst, 7, 10);
    
    GMLReal negaspeed = selfReal(inst, genfireCache.negaspeed);
    inst->speed -= (float)negaspeed;
    Instance_computeComponentsFromSpeed(inst);
}





static struct { int32_t dr, turntimer; bool ready; } firestormCache = {.ready=false};
static void initFirestormCache(VMContext* ctx, DataWin* dw) {
    firestormCache.dr = findSelfVarId(dw, "dr");
    firestormCache.turntimer = findGlobalVarId(ctx, "turntimer");
    firestormCache.ready = (firestormCache.dr >= 0 && firestormCache.turntimer >= 0);
}
static void native_firestormgen_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!firestormCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;
    GMLReal dr = selfReal(inst, firestormCache.dr);
    if (dr < 0.5) dr += 0.1;
    GMLReal turntimer = globalReal(ctx, firestormCache.turntimer);
    if (turntimer < 6) dr -= 0.2;
    Instance_setSelfVar(inst, firestormCache.dr, RValue_makeReal(dr));
    
    float savedAlpha = r->drawAlpha;
    r->drawAlpha = (float)dr;
    r->drawColor = 0;
    r->vtable->drawRectangle(r, -10.0f, -10.0f, 999.0f, 999.0f, 0, (float)dr, false);
    r->drawAlpha = savedAlpha;
    r->drawColor = 0xFFFFFF;
    if (turntimer <= 0) Runner_destroyInstance(runner, inst);
}







static void native_sidedfire_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!chasefire2Cache.ready) return;
    
    updateBorderPosCache(ctx, runner);
    GMLReal border3 = borderPosCache.border3;
    GMLReal border2 = borderPosCache.border2;
    if (inst->y > (float)border3 && inst->vspeed > 0) {
        Runner_destroyInstance(runner, inst); return;
    }
    DataWin* dw = ctx->dataWin;
    Sprite* spr = (inst->spriteIndex >= 0 && (uint32_t)inst->spriteIndex < dw->sprt.count)
                  ? &dw->sprt.sprites[inst->spriteIndex] : NULL;
    float sprH = spr ? (float)spr->height * inst->imageYscale : 0;
    if (inst->y < (float)(border2 - sprH) && inst->vspeed < 0) {
        Runner_destroyInstance(runner, inst); return;
    }
    if (checkHeartCollision(runner, inst))
        Runner_executeEvent(runner, inst, 7, 10);
}






#define SPEAR_MAX_ANGLE_VARIDS 8
static struct {
    int32_t siner, color_v, xhand1, yhand1, rdistx, rdisty;
    int32_t armtest, debuggo;
    
    
    
    int32_t angleCandidates[SPEAR_MAX_ANGLE_VARIDS];
    int32_t angleCandidateCount;
    int32_t angleResolvedForInstance;  
    int32_t angleResolvedVarId;
    int32_t objBody, objAsgorearm; 
    bool ready;
} spearCache = { .ready = false };


static int32_t findAllSelfVarIds(DataWin* dw, const char* name, int32_t* outArr, int32_t maxCount) {
    int32_t count = 0;
    forEach(Variable, v, dw->vari.variables, dw->vari.variableCount) {
        if (v->varID >= 0 && v->instanceType != INSTANCE_GLOBAL && strcmp(v->name, name) == 0) {
            bool dup = false;
            for (int32_t k = 0; k < count; k++) if (outArr[k] == v->varID) { dup = true; break; }
            if (!dup && count < maxCount) outArr[count++] = v->varID;
        }
    }
    return count;
}



static int32_t resolveSelfVarIdForInst(Instance* inst, const int32_t* candidates, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        if (hmgeti(inst->selfVars, candidates[i]) >= 0) return candidates[i];
    }
    return (count > 0) ? candidates[0] : -1;  
}

static void initSpearCache(DataWin* dw) {
    spearCache.siner = findSelfVarId(dw, "siner");
    spearCache.color_v = findSelfVarId(dw, "color");
    spearCache.xhand1 = findSelfVarId(dw, "xhand1");
    spearCache.yhand1 = findSelfVarId(dw, "yhand1");
    spearCache.rdistx = findSelfVarId(dw, "rdistx");
    spearCache.rdisty = findSelfVarId(dw, "rdisty");
    spearCache.armtest = findSelfVarId(dw, "armtest");
    spearCache.debuggo = findSelfVarId(dw, "debuggo");
    spearCache.angleCandidateCount = findAllSelfVarIds(dw, "angle",
                                                       spearCache.angleCandidates,
                                                       SPEAR_MAX_ANGLE_VARIDS);
    spearCache.angleResolvedForInstance = -1;
    spearCache.angleResolvedVarId = -1;
    spearCache.objBody = findObjectIndex(dw, "obj_asgoreb_body");
    spearCache.objAsgorearm = 489;
    spearCache.ready = (spearCache.siner >= 0 && spearCache.angleCandidateCount > 0 &&
                        spearCache.color_v >= 0 && spearCache.xhand1 >= 0);
    if (spearCache.angleCandidateCount > 1) {
        fprintf(stderr, "NativeScripts: spear 'angle' has %d VARI entries — per-instance resolve enabled\n",
                spearCache.angleCandidateCount);
    }
}

static void native_asgorespear_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!spearCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    

    
    GMLReal siner = selfReal(inst, spearCache.siner) + 1.0;
    Instance_setSelfVar(inst, spearCache.siner, RValue_makeReal(siner));

    
    GMLReal sinVal = GMLReal_sin(siner / 15.0);
    inst->y += (float)(sinVal * 0.3);

    
    int32_t angleVarId = spearCache.angleResolvedVarId;
    if (spearCache.angleResolvedForInstance != (int32_t)inst->instanceId || angleVarId < 0) {
        angleVarId = resolveSelfVarIdForInst(inst, spearCache.angleCandidates,
                                             spearCache.angleCandidateCount);
        spearCache.angleResolvedForInstance = (int32_t)inst->instanceId;
        spearCache.angleResolvedVarId = angleVarId;
    }

    
    GMLReal angle = (angleVarId >= 0) ? selfReal(inst, angleVarId) : 0.0;
    angle += sinVal * 0.02;
    if (angleVarId >= 0) Instance_setSelfVar(inst, angleVarId, RValue_makeReal(angle));

    
    GMLReal angleRad = angle * (M_PI / 180.0);
    GMLReal xhand1 = GMLReal_cos(angleRad) * 55.0;
    GMLReal yhand1 = -GMLReal_sin(angleRad) * 55.0; 
    Instance_setSelfVar(inst, spearCache.xhand1, RValue_makeReal(xhand1));
    Instance_setSelfVar(inst, spearCache.yhand1, RValue_makeReal(yhand1));

    
    GMLReal rdistx = inst->x + xhand1 * 2.0;
    GMLReal rdisty = inst->y + yhand1 * 2.0;
    Instance_setSelfVar(inst, spearCache.rdistx, RValue_makeReal(rdistx));
    Instance_setSelfVar(inst, spearCache.rdisty, RValue_makeReal(rdisty));

    
    GMLReal armtest = selfReal(inst, spearCache.armtest);
    if ((int32_t)armtest == 1) {
        Instance* armInst = findInstanceByObject(runner, spearCache.objAsgorearm);
        if (armInst) {
            Instance* body = (spearCache.objBody >= 0) ? findInstanceByObject(runner, spearCache.objBody) : NULL;
            if (body && asgBodyCache.ready) {
                
                GMLReal bpx5 = RValue_toReal(selfArrayGet(body, asgBodyCache.partx, 5));
                GMLReal bpy5 = RValue_toReal(selfArrayGet(body, asgBodyCache.party, 5));
                float p1x = (float)(bpx5 + 14.0 + body->x);
                float p1y = (float)(bpy5 + 64.0 + body->y);
                float dx = (float)(inst->x - xhand1) - p1x;
                float dy = (float)(inst->y - yhand1) - p1y;
                float armlen = sqrtf(dx*dx + dy*dy);
                float armang = atan2f(-dy, dx) * (180.0f / (float)M_PI);
                if (armang < 0) armang += 360.0f; 
                float armsize = armlen / 40.0f;
                if (armsize < 0.35f) armsize = 0;
                Renderer_drawSpriteExt(r, 633, 0, p1x, p1y, armsize*2.0f, 2.0f, armang, 0xFFFFFF, 1.0f);

                
                GMLReal bpx4 = RValue_toReal(selfArrayGet(body, asgBodyCache.partx, 4));
                GMLReal bpy4 = RValue_toReal(selfArrayGet(body, asgBodyCache.party, 4));
                p1x = (float)(bpx4 + 34.0 + body->x);
                p1y = (float)(bpy4 + 64.0 + body->y);
                float rdx = (float)rdistx, rdy = (float)rdisty;
                dx = rdx - p1x; dy = rdy - p1y;
                armlen = sqrtf(dx*dx + dy*dy);
                if (armlen > 100.0f) {
                    float armoff = (armlen - 100.0f) / 2.0f;
                    rdx = (float)(inst->x + GMLReal_cos(angleRad) * (55.0 - armoff) * 2.0);
                    rdy = (float)(inst->y + (-GMLReal_sin(angleRad)) * (55.0 - armoff) * 2.0);
                    dx = rdx - p1x; dy = rdy - p1y;
                    armlen = sqrtf(dx*dx + dy*dy);
                }
                armang = atan2f(-dy, dx) * (180.0f / (float)M_PI);
                if (armang < 0) armang += 360.0f; 
                if (armang > 100.0f) p1y -= 12.0f;
                armsize = armlen / 40.0f;
                if (armsize < 0.6f) armsize = 0;
                Renderer_drawSpriteExt(r, 633, 0, p1x, p1y, armsize*2.0f, 2.0f, armang, 0xFFFFFF, 1.0f);
            }
        }
    }

    
    uint32_t color = (uint32_t)selfReal(inst, spearCache.color_v);
    float fAngle = (float)angle;
    int32_t imgIdx = (int32_t)inst->imageIndex;
    Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, inst->x, inst->y, 2.0f, 2.0f, fAngle, color, 1.0f);
    Renderer_drawSpriteExt(r, 642, imgIdx, (float)rdistx, (float)rdisty, 2.0f, 2.0f, fAngle, 0xFFFFFF, 1.0f);
    Renderer_drawSpriteExt(r, 643, imgIdx, inst->x - (float)xhand1, inst->y - (float)yhand1, 2.0f, 2.0f, fAngle, 0xFFFFFF, 1.0f);
}










static struct {
    
    int32_t drawrect, drawbinfo, xwrite;
    
    int32_t turntimer, bmenuno, myfight, mnfight, language;
    int32_t lv, hp, maxhp, km;
    int32_t monster, monstername, monsterhp, monstermaxhp;
    int32_t flag, item, itemnameb, idealborder, bmenucoord;
    int32_t charname, osflavor;
    
    int32_t objUborder, objRborder, objDborder;
    bool ready;
} bcCache = { .ready = false };

static void initBcCache(VMContext* ctx, DataWin* dw) {
    bcCache.drawrect = findSelfVarId(dw, "drawrect");
    bcCache.drawbinfo = findSelfVarId(dw, "drawbinfo");
    bcCache.xwrite = findSelfVarId(dw, "xwrite");
    bcCache.turntimer = findGlobalVarId(ctx, "turntimer");
    bcCache.bmenuno = findGlobalVarId(ctx, "bmenuno");
    bcCache.myfight = findGlobalVarId(ctx, "myfight");
    bcCache.mnfight = findGlobalVarId(ctx, "mnfight");
    bcCache.language = findGlobalVarId(ctx, "language");
    bcCache.lv = findGlobalVarId(ctx, "lv");
    bcCache.hp = findGlobalVarId(ctx, "hp");
    bcCache.maxhp = findGlobalVarId(ctx, "maxhp");
    bcCache.km = findGlobalVarId(ctx, "km");
    bcCache.monster = findGlobalVarId(ctx, "monster");
    bcCache.monstername = findGlobalVarId(ctx, "monstername");
    bcCache.monsterhp = findGlobalVarId(ctx, "monsterhp");
    bcCache.monstermaxhp = findGlobalVarId(ctx, "monstermaxhp");
    bcCache.flag = findGlobalVarId(ctx, "flag");
    bcCache.item = findGlobalVarId(ctx, "item");
    bcCache.itemnameb = findGlobalVarId(ctx, "itemnameb");
    bcCache.idealborder = findGlobalVarId(ctx, "idealborder");
    bcCache.bmenucoord = findGlobalVarId(ctx, "bmenucoord");
    bcCache.charname = findGlobalVarId(ctx, "charname");
    bcCache.osflavor = findGlobalVarId(ctx, "osflavor");
    bcCache.objUborder = findObjectIndex(dw, "obj_uborder");
    bcCache.objRborder = findObjectIndex(dw, "obj_rborder");
    bcCache.objDborder = findObjectIndex(dw, "obj_dborder");
    bcCache.ready = (bcCache.drawrect >= 0 && bcCache.drawbinfo >= 0 &&
                     bcCache.turntimer >= 0 && bcCache.hp >= 0 && bcCache.maxhp >= 0 &&
                     bcCache.lv >= 0 && bcCache.charname >= 0);
}


static inline void drawFilledRect(Renderer* r, float x1, float y1, float x2, float y2) {
    if (x1 > x2) { float t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { float t = y1; y1 = y2; y2 = t; }
    r->vtable->drawRectangle(r, x1, y1, x2, y2, r->drawColor, r->drawAlpha, false);
}


static inline void nativeDrawText(Runner* runner, Renderer* r, float x, float y, const char* text) {
    char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, text);
    r->vtable->drawText(r, processed, x, y, 1.0f, 1.0f, 0.0f);
    free(processed);
}


static inline float nativeStringWidth(Runner* runner, Renderer* r, const char* text) {
    int32_t fontIndex = r->drawFont;
    if (0 > fontIndex || r->dataWin->font.count <= (uint32_t)fontIndex) return 0.0f;
    Font* font = &r->dataWin->font.fonts[fontIndex];
    char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, text);
    int32_t textLen = (int32_t)strlen(processed);
    float maxWidth = 0;
    int32_t lineStart = 0;
    while (textLen >= lineStart) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(processed[lineEnd]))
            lineEnd++;
        float w = TextUtils_measureLineWidth(font, processed + lineStart, lineEnd - lineStart);
        if (w > maxWidth) maxWidth = w;
        lineStart = lineEnd + 1;
    }
    free(processed);
    return maxWidth;
}


static inline void nativeSetFont(Renderer* r, VMContext* ctx, int32_t fontNum) {
    if (bcCache.language >= 0) {
        const char* lang = globalString(ctx, bcCache.language);
        if (lang && strcmp(lang, "ja") == 0) {
            if (fontNum == 1) fontNum = 13;
            else if (fontNum == 2) fontNum = 14;
            else if (fontNum == 4) fontNum = 17;
        }
    }
    r->drawFont = fontNum;
}


static inline uint32_t nativeMergeColor(uint32_t col1, uint32_t col2, float amount) {
    float inv = 1.0f - amount;
    int32_t r = (int32_t)((col1 & 0xFF) * inv + (col2 & 0xFF) * amount);
    int32_t g = (int32_t)(((col1 >> 8) & 0xFF) * inv + ((col2 >> 8) & 0xFF) * amount);
    int32_t b = (int32_t)(((col1 >> 16) & 0xFF) * inv + ((col2 >> 16) & 0xFF) * amount);
    return ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
}

static void native_battlecontroller_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!bcCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    
    GMLReal turntimer = globalReal(ctx, bcCache.turntimer);
    if (turntimer > 0) {
        inst->depth = -1000;
        r->drawColor = 0xFF; 
        RValue_free(&ctx->globalVars[bcCache.turntimer]);
        ctx->globalVars[bcCache.turntimer] = RValue_makeReal(turntimer - 1);
    }

    
    
    Instance* uborder = (bcCache.objUborder >= 0) ? findInstanceByObject(runner, bcCache.objUborder) : NULL;
    if (uborder) {
        inst->depth = 5;
        r->drawColor = 0;
        int32_t drawrect = selfInt(inst, bcCache.drawrect);
        if (drawrect == 1) {
            Instance* rborder = (bcCache.objRborder >= 0) ? findInstanceByObject(runner, bcCache.objRborder) : NULL;
            Instance* dborder = (bcCache.objDborder >= 0) ? findInstanceByObject(runner, bcCache.objDborder) : NULL;
            if (rborder && dborder) {
                drawFilledRect(r, uborder->x + 5, uborder->y + 5, rborder->x, dborder->y);
            }
        }
    }

    
    const char* lang = (bcCache.language >= 0) ? globalString(ctx, bcCache.language) : "";
    bool isJa = (lang && strcmp(lang, "ja") == 0);

    
    uint32_t bgColor = runner->backgroundColor;
    int32_t drawbinfo = selfInt(inst, bcCache.drawbinfo);
    if (bgColor != 0xFFFFFF && drawbinfo == 1) {
        
        r->drawColor = 0xFFFFFF; 

        
        float namex = 30.0f, namey = 400.0f;
        const char* charname = (bcCache.charname >= 0) ? globalString(ctx, bcCache.charname) : "";

        int32_t useFont = 7;
        float nameY = namey;
        if (isJa) {
            useFont = 12;
            nameY += 3;
        } else {
            
            for (int32_t i = 0; charname[i]; i++) {
                
                uint8_t ch = (uint8_t)charname[i];
                if (ch >= 0xE3) { 
                    useFont = 12;
                    nameY += 3;
                    break;
                }
            }
        }
        r->drawFont = useFont;
        r->drawHalign = 0;
        r->drawValign = 0;
        nativeDrawText(runner, r, namex, nameY, charname);
        float namewidth = nativeStringWidth(runner, r, charname);

        
        nativeSetFont(r, ctx, 7);
        int32_t lv = (int32_t)globalReal(ctx, bcCache.lv);
        char lvBuf[64];
        snprintf(lvBuf, sizeof(lvBuf), "   LV %d", lv);
        nativeDrawText(runner, r, namex + namewidth, 400.0f, lvBuf);

        GMLReal hp = globalReal(ctx, bcCache.hp);
        GMLReal maxhp = globalReal(ctx, bcCache.maxhp);
        int32_t flag271 = 0;
        if (bcCache.flag >= 0) {
            flag271 = (int32_t)getGlobalArray(ctx, bcCache.flag, 271);
        }

        if (flag271 == 0) {
            
            r->drawColor = 0xFF; 
            drawFilledRect(r, 275.0f, 400.0f, 275.0f + (float)(maxhp * 1.2), 420.0f);
            r->drawColor = 0xFFFF; 
            drawFilledRect(r, 275.0f, 400.0f, 275.0f + (float)(hp * 1.2), 420.0f);
            r->drawColor = 0xFFFFFF; 
            nativeSetFont(r, ctx, 7);

            const char* hpwrite;
            char hpBuf[64];
            int32_t flag509 = (bcCache.flag >= 0) ? (int32_t)getGlobalArray(ctx, bcCache.flag, 509) : 0;
            if (flag509 == 1) hpwrite = "00.001";
            else if (flag509 == 2) hpwrite = "00.0001";
            else if (flag509 == 3) hpwrite = "00.000001";
            else if (flag509 == 4) hpwrite = "00.0000000001";
            else if (hp < 0) hpwrite = "0";
            else if (hp < 10) { snprintf(hpBuf, sizeof(hpBuf), "0%d", (int32_t)hp); hpwrite = hpBuf; }
            else { snprintf(hpBuf, sizeof(hpBuf), "%d", (int32_t)hp); hpwrite = hpBuf; }

            char hpTextBuf[128];
            snprintf(hpTextBuf, sizeof(hpTextBuf), "%s / %d", hpwrite, (int32_t)maxhp);
            nativeDrawText(runner, r, 290.0f + (float)(maxhp * 1.2), 400.0f, hpTextBuf);
        } else {
            
            uint32_t mergedColor = nativeMergeColor(0xFF, 128, 0.5f);
            r->drawColor = mergedColor;
            drawFilledRect(r, 255.0f, 400.0f, 255.0f + (float)(maxhp * 1.2), 420.0f);
            r->drawColor = 0xFFFF; 
            drawFilledRect(r, 255.0f, 400.0f, 255.0f + (float)(hp * 1.2), 420.0f);

            GMLReal km = (bcCache.km >= 0) ? globalReal(ctx, bcCache.km) : 0;
            if (km > 40) km = 40;
            if (km >= hp) km = hp - 1;

            r->drawColor = 0xFF00FF; 
            drawFilledRect(r, 255.0f + (float)(hp * 1.2), 400.0f,
                          (255.0f + (float)(hp * 1.2)) - (float)(km * 1.2), 420.0f);

            
            Renderer_drawSprite(r, 710, 0, 265.0f + (float)(maxhp * 1.2), 405.0f);

            r->drawColor = 0xFFFFFF; 
            nativeSetFont(r, ctx, 7);

            if (km > 0) r->drawColor = 0xFF00FF; 

            char hpBuf[64];
            const char* hpwrite;
            if (hp < 0) hpwrite = "0";
            else if (hp < 10) { snprintf(hpBuf, sizeof(hpBuf), "0%d", (int32_t)hp); hpwrite = hpBuf; }
            else { snprintf(hpBuf, sizeof(hpBuf), "%d", (int32_t)hp); hpwrite = hpBuf; }

            char hpTextBuf[128];
            snprintf(hpTextBuf, sizeof(hpTextBuf), "%s / %d", hpwrite, (int32_t)maxhp);
            nativeDrawText(runner, r, 305.0f + (float)(maxhp * 1.2), 400.0f, hpTextBuf);
            r->drawColor = 0xFFFFFF;

            
            
            Instance* obj184 = findInstanceByObject(runner, 184);
            if (obj184) Runner_destroyInstance(runner, obj184);

            
            Renderer_drawSprite(r, 23, 0, 220.0f, 400.0f);
        }
        
    }

    
    GMLReal bmenuno = (bcCache.bmenuno >= 0) ? globalReal(ctx, bcCache.bmenuno) : 0;
    GMLReal myfight = (bcCache.myfight >= 0) ? globalReal(ctx, bcCache.myfight) : 0;
    GMLReal mnfight = (bcCache.mnfight >= 0) ? globalReal(ctx, bcCache.mnfight) : 0;

    if ((int32_t)bmenuno == 1 && (int32_t)myfight == 0 && (int32_t)mnfight == 0) {
        float maxwidth = 0;
        for (int i = 0; i < 3; i++) {
            GMLReal monsterVal = (bcCache.monster >= 0) ? getGlobalArray(ctx, bcCache.monster, i) : 0;
            if ((int32_t)monsterVal == 1) {
                
                int64_t nameKey = ((int64_t)bcCache.monstername << 32) | (uint32_t)i;
                ptrdiff_t nameIdx = hmgeti(ctx->globalArrayMap, nameKey);
                const char* name = "";
                if (nameIdx >= 0 && ctx->globalArrayMap[nameIdx].value.type == RVALUE_STRING)
                    name = ctx->globalArrayMap[nameIdx].value.string;

                float width;
                if (isJa) {
                    width = 0;
                    for (int32_t j = 0; name[j]; ) {
                        
                        uint32_t ch;
                        uint8_t b = (uint8_t)name[j];
                        if (b < 0x80) { ch = b; j++; }
                        else if (b < 0xE0) { ch = ((b & 0x1F) << 6) | (name[j+1] & 0x3F); j += 2; }
                        else if (b < 0xF0) { ch = ((b & 0x0F) << 12) | ((name[j+1] & 0x3F) << 6) | (name[j+2] & 0x3F); j += 3; }
                        else { ch = 0; j++; } 

                        if (ch == 32 || ch >= 65377) width += 13;
                        else if (ch < 8192) width += 16;
                        else width += 26;
                    }
                } else {
                    width = (float)(strlen(name) * 16);
                }
                if (width > maxwidth) maxwidth = width;
            }
        }

        float xwrite = 190.0f + maxwidth;
        Instance_setSelfVar(inst, bcCache.xwrite, RValue_makeReal(xwrite));

        for (int i = 0; i < 3; i++) {
            GMLReal monsterVal = (bcCache.monster >= 0) ? getGlobalArray(ctx, bcCache.monster, i) : 0;
            
            bool has520 = (findInstanceByObject(runner, 520) != NULL);
            if ((int32_t)monsterVal == 1 && !has520) {
                r->drawColor = 0xFF; 
                int32_t lineheight = isJa ? 36 : 32;
                float y_start = 280.0f;
                drawFilledRect(r, xwrite, y_start + (i * lineheight), xwrite + 100, y_start + (i * lineheight) + 16);
                r->drawColor = 0xFF00; 
                GMLReal mhp = (bcCache.monsterhp >= 0) ? getGlobalArray(ctx, bcCache.monsterhp, i) : 0;
                GMLReal mmhp = (bcCache.monstermaxhp >= 0) ? getGlobalArray(ctx, bcCache.monstermaxhp, i) : 1;
                if (mmhp == 0) mmhp = 1;
                drawFilledRect(r, xwrite, y_start + (i * lineheight),
                              xwrite + (float)((mhp / mmhp) * 100.0), y_start + (i * lineheight) + 16);
            }
        }
    }

    
    if (isJa && (int32_t)bmenuno >= 3 && bmenuno < 4.0 && (int32_t)myfight == 0 && (int32_t)mnfight == 0) {
        int32_t first = ((int32_t)bmenuno - 3) * 8;
        nativeSetFont(r, ctx, 1);
        r->drawColor = 0xFFFFFF;

        float xx = (bcCache.idealborder >= 0) ? (float)getGlobalArray(ctx, bcCache.idealborder, 0) + 20.0f : 20.0f;
        float yy = (bcCache.idealborder >= 0) ? (float)getGlobalArray(ctx, bcCache.idealborder, 2) + 20.0f : 20.0f;

        
        RValue gettextArgs[] = { RValue_makeString("item_menub_header") };
        RValue headerRv = callBuiltin(ctx, "scr_gettext", gettextArgs, 1);
        const char* lineheader = (headerRv.type == RVALUE_STRING) ? headerRv.string : "";

        for (int i = 0; i < 3; i++) {
            GMLReal itemVal = (bcCache.item >= 0) ? getGlobalArray(ctx, bcCache.item, first + i) : 0;
            if ((int32_t)itemVal == 0) break;
            
            int64_t inKey = ((int64_t)bcCache.itemnameb << 32) | (uint32_t)(first + i);
            ptrdiff_t inIdx = hmgeti(ctx->globalArrayMap, inKey);
            const char* itemname = "";
            if (inIdx >= 0 && ctx->globalArrayMap[inIdx].value.type == RVALUE_STRING)
                itemname = ctx->globalArrayMap[inIdx].value.string;

            char textBuf[256];
            snprintf(textBuf, sizeof(textBuf), "%s%s", lineheader, itemname);
            nativeDrawText(runner, r, xx, yy + (i * 36.0f), textBuf);
        }
        RValue_free(&headerRv);

        
        int32_t num_items = 8;
        while (num_items > 0) {
            GMLReal itemVal = (bcCache.item >= 0) ? getGlobalArray(ctx, bcCache.item, num_items - 1) : 0;
            if ((int32_t)itemVal != 0) break;
            num_items--;
        }

        if (num_items > 3) {
            float bx = (bcCache.idealborder >= 0) ? (float)getGlobalArray(ctx, bcCache.idealborder, 1) - 30.0f : 270.0f;
            float border2 = (bcCache.idealborder >= 0) ? (float)getGlobalArray(ctx, bcCache.idealborder, 2) : 0;
            float border3 = (bcCache.idealborder >= 0) ? (float)getGlobalArray(ctx, bcCache.idealborder, 3) : 0;
            float by = floorf((border2 + border3) / 2.0f) - (5.0f * (2 + num_items));

            
            
            float arrow_yofs = 0;
            
            float tmod = (float)(runner->frameCount % 30) / 30.0f;
            if (tmod > 0.5f) tmod = 0.5f;
            arrow_yofs = roundf(tmod * 6.0f);

            if (first > 0)
                Renderer_drawSprite(r, 43, 0, bx, by - arrow_yofs);

            by += 10.0f;

            int32_t bmenucoord3 = (bcCache.bmenucoord >= 0) ? (int32_t)getGlobalArray(ctx, bcCache.bmenucoord, 3) : 0;
            for (int i = 0; i < num_items; i++) {
                int32_t spr = ((first + bmenucoord3) == i) ? 45 : 44;
                Renderer_drawSprite(r, spr, 0, bx, by);
                by += 10.0f;
            }

            if ((first + 3) < num_items) {
                
                int32_t tpagIdx = Renderer_resolveTPAGIndex(r->dataWin, 43, 0);
                if (tpagIdx >= 0) {
                    Sprite* spr = &r->dataWin->sprt.sprites[43];
                    r->vtable->drawSprite(r, tpagIdx, bx, by + 10.0f + arrow_yofs,
                                          (float)spr->originX, (float)spr->originY,
                                          1.0f, -1.0f, 0.0f, 0xFFFFFF, 1.0f);
                }
            }
        }
    }
}


static void native_time_Draw76(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)runner; (void)inst;
    static int32_t cachedWindowScale = -1;
    if (cachedWindowScale < 0) cachedWindowScale = findGlobalVarId(ctx, "window_scale");
    if (cachedWindowScale >= 0) {
        RValue_free(&ctx->globalVars[cachedWindowScale]);
        ctx->globalVars[cachedWindowScale] = RValue_makeReal(1.0);
    }
}


static void native_time_Draw77(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)runner; (void)inst;
    static int32_t cachedXofs = -1, cachedYofs = -1;
    if (cachedXofs < 0) { cachedXofs = findGlobalVarId(ctx, "window_xofs"); cachedYofs = findGlobalVarId(ctx, "window_yofs"); }
    if (cachedXofs >= 0) { RValue_free(&ctx->globalVars[cachedXofs]); ctx->globalVars[cachedXofs] = RValue_makeReal(0.0); }
    if (cachedYofs >= 0) { RValue_free(&ctx->globalVars[cachedYofs]); ctx->globalVars[cachedYofs] = RValue_makeReal(0.0); }
}







static void native_screen_Step1(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner; (void)inst;
    
}








static struct {
    int32_t dsprite, rsprite, usprite, lsprite;
    int32_t crumpet, strumpet, trumpet, movement, moving, turned;
    int32_t timeLeft, timeRight, timeUp, timeDown;  
    int32_t gFacing, gInbattle, gDebug, gFlag, gControlPressed;
    int32_t objTime;
    int32_t objWriter;   
    int32_t objTrigger;  
    int32_t objCrumpet;  
    int32_t mainCharaCodeId;
    
    uint64_t cachedObjTimeFrame;
    Instance* cachedObjTime;
    bool ready;
} maincharaCache = { .ready = false, .cachedObjTimeFrame = UINT64_MAX, .cachedObjTime = NULL };

static void initMaincharaCache(VMContext* ctx, DataWin* dw) {
    maincharaCache.dsprite = findSelfVarId(dw, "dsprite");
    maincharaCache.rsprite = findSelfVarId(dw, "rsprite");
    maincharaCache.usprite = findSelfVarId(dw, "usprite");
    maincharaCache.lsprite = findSelfVarId(dw, "lsprite");
    maincharaCache.crumpet = findSelfVarId(dw, "crumpet");
    maincharaCache.strumpet = findSelfVarId(dw, "strumpet");
    maincharaCache.trumpet = findSelfVarId(dw, "trumpet");
    maincharaCache.movement = findSelfVarId(dw, "movement");
    maincharaCache.moving = findSelfVarId(dw, "moving");
    maincharaCache.turned = findSelfVarId(dw, "turned");
    
    maincharaCache.timeLeft = findSelfVarId(dw, "left");
    maincharaCache.timeRight = findSelfVarId(dw, "right");
    maincharaCache.timeUp = findSelfVarId(dw, "up");
    maincharaCache.timeDown = findSelfVarId(dw, "down");
    maincharaCache.gFacing = findGlobalVarId(ctx, "facing");
    maincharaCache.gInbattle = findGlobalVarId(ctx, "inbattle");
    maincharaCache.gDebug = findGlobalVarId(ctx, "debug");
    maincharaCache.gFlag = findGlobalVarId(ctx, "flag");
    maincharaCache.gControlPressed = findGlobalVarId(ctx, "control_pressed");
    maincharaCache.objTime = findObjectIndex(dw, "obj_time");
    maincharaCache.objWriter = 143;
    maincharaCache.objTrigger = 795;
    maincharaCache.objCrumpet = 822;
    maincharaCache.mainCharaCodeId = -1;
    for (uint32_t ci = 0; ci < dw->code.count; ci++) {
        if (strcmp(dw->code.entries[ci].name, "gml_Object_obj_mainchara_Step_0") == 0) {
            maincharaCache.mainCharaCodeId = (int32_t)ci; break;
        }
    }
    maincharaCache.ready = (maincharaCache.dsprite >= 0 && maincharaCache.timeLeft >= 0 &&
                            maincharaCache.gFacing >= 0 && maincharaCache.gControlPressed >= 0 &&
                            maincharaCache.objTime >= 0 && maincharaCache.movement >= 0);
}



static inline bool mainchara_controlPressed(VMContext* ctx, int32_t idx) {
    if (maincharaCache.gControlPressed < 0 || idx < 0 || idx > 2) return false;
    int64_t k = ((int64_t)maincharaCache.gControlPressed << 32) | (uint32_t)idx;
    ptrdiff_t i = hmgeti(ctx->globalArrayMap, k);
    if (i < 0) return false;
    return RValue_toReal(ctx->globalArrayMap[i].value) != 0;
}


static inline Instance* mainchara_getObjTime(Runner* runner) {
    uint64_t frame = (uint64_t)runner->frameCount;
    if (maincharaCache.cachedObjTimeFrame != frame || maincharaCache.cachedObjTime == NULL) {
        maincharaCache.cachedObjTime = findInstanceByObject(runner, maincharaCache.objTime);
        maincharaCache.cachedObjTimeFrame = frame;
    }
    return maincharaCache.cachedObjTime;
}



static bool mainchara_fireCollisionRectEvent(VMContext* ctx, Runner* runner, Instance* self,
                                             InstanceBBox* selfBBox, int32_t targetObj, int32_t userIdx) {
    int32_t count = (int32_t)arrlen(runner->instances);
    float x1 = selfBBox->left, y1 = selfBBox->top;
    float x2 = selfBBox->right, y2 = selfBBox->bottom;
    for (int32_t i = 0; i < count; i++) {
        Instance* inst = runner->instances[i];
        if (!inst->active) continue;
        if (!Collision_matchesTarget(ctx->dataWin, inst, targetObj)) continue;
        InstanceBBox b = Collision_computeBBox(ctx->dataWin, inst);
        if (!b.valid) continue;
        if (x1 >= b.right || b.left >= x2 || y1 >= b.bottom || b.top >= y2) continue;
        Instance* saved = (Instance*)ctx->currentInstance;
        ctx->currentInstance = inst;
        Runner_executeEvent(runner, inst, 7, OTHER_USER0 + userIdx);
        ctx->currentInstance = saved;
        (void)self;
        return true;
    }
    return false;
}

static void native_mainchara_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!maincharaCache.ready) return;

    
    
    if (maincharaCache.gInbattle >= 0 && globalReal(ctx, maincharaCache.gInbattle) == 1.0) {
        if (maincharaCache.mainCharaCodeId >= 0) {
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue r = VM_executeCode(ctx, maincharaCache.mainCharaCodeId);
            RValue_free(&r);
            ctx->currentInstance = saved;
        }
        return;
    }

    
    int32_t facing = (int32_t)globalReal(ctx, maincharaCache.gFacing);
    if (facing == 0) inst->spriteIndex = selfInt(inst, maincharaCache.dsprite);
    else if (facing == 1) inst->spriteIndex = selfInt(inst, maincharaCache.rsprite);
    else if (facing == 2) inst->spriteIndex = selfInt(inst, maincharaCache.usprite);
    else if (facing == 3) inst->spriteIndex = selfInt(inst, maincharaCache.lsprite);

    
    InstanceBBox selfBBox = Collision_computeBBox(ctx->dataWin, inst);
    if (!selfBBox.valid) {
        
        return;
    }

    
    int32_t crumpetResult = 2;
    {
        float px = selfBBox.left - 3.0f;
        float py = selfBBox.top - 3.0f;
        bool hit = false;
        int32_t count = (int32_t)arrlen(runner->instances);
        for (int32_t i = 0; i < count; i++) {
            Instance* other = runner->instances[i];
            if (!other->active) continue;
            if (other == inst) continue; 
            if (!Collision_matchesTarget(ctx->dataWin, other, maincharaCache.objCrumpet)) continue;
            InstanceBBox ob = Collision_computeBBox(ctx->dataWin, other);
            if (!ob.valid) continue;
            if (ob.left > px || px >= ob.right || ob.top > py || py >= ob.bottom) continue;
            hit = true;
            break;
        }
        crumpetResult = hit ? 2 : 1;
    }
    Instance_setSelfVar(inst, maincharaCache.crumpet, RValue_makeReal((GMLReal)crumpetResult));
    Instance_setSelfVar(inst, maincharaCache.strumpet, RValue_makeReal((GMLReal)selfBBox.top));
    Instance_setSelfVar(inst, maincharaCache.trumpet, RValue_makeReal((GMLReal)selfBBox.left));

    
    Instance* objTime = mainchara_getObjTime(runner);
    bool tLeft = objTime ? (selfReal(objTime, maincharaCache.timeLeft) != 0) : false;
    bool tRight = objTime ? (selfReal(objTime, maincharaCache.timeRight) != 0) : false;
    bool tUp = objTime ? (selfReal(objTime, maincharaCache.timeUp) != 0) : false;
    bool tDown = objTime ? (selfReal(objTime, maincharaCache.timeDown) != 0) : false;

    int32_t movement = selfInt(inst, maincharaCache.movement);
    GMLReal debugV = (maincharaCache.gDebug >= 0) ? globalReal(ctx, maincharaCache.gDebug) : 0;
    bool debugOn = (debugV == 1);
    BuiltinFunc kbCheck = debugOn ? VMBuiltins_find("keyboard_check") : NULL;

    
    if (tLeft && movement == 1) {
        int32_t turned = 1;
        if (inst->xprevious == (inst->x + 3.0f)) inst->x -= 2.0f;
        else inst->x -= 3.0f;
        int32_t moving = selfInt(inst, maincharaCache.moving);
        if (moving != 1) inst->imageIndex = 1.0f;
        Instance_setSelfVar(inst, maincharaCache.moving, RValue_makeReal(1.0));
        if (debugOn && kbCheck) {
            RValue a = RValue_makeReal(8.0);
            RValue r = kbCheck(ctx, &a, 1);
            if (RValue_toReal(r) != 0) inst->x -= 5.0f;
            RValue_free(&r);
        }
        inst->imageSpeed = 0.2f;
        if (tUp && facing == 2) turned = 0;
        if (tDown && facing == 0) turned = 0;
        if (turned == 1) {
            globalSet(ctx, maincharaCache.gFacing, RValue_makeReal(3.0));
            facing = 3;
        }
        Instance_setSelfVar(inst, maincharaCache.turned, RValue_makeReal((GMLReal)turned));
    }

    
    if (tUp && movement == 1) {
        int32_t turned = 1;
        inst->y -= 3.0f;
        if (debugOn && kbCheck) {
            RValue a = RValue_makeReal(8.0);
            RValue r = kbCheck(ctx, &a, 1);
            if (RValue_toReal(r) != 0) inst->y -= 5.0f;
            RValue_free(&r);
        }
        int32_t moving = selfInt(inst, maincharaCache.moving);
        if (moving != 1) inst->imageIndex = 1.0f;
        Instance_setSelfVar(inst, maincharaCache.moving, RValue_makeReal(1.0));
        inst->imageSpeed = 0.2f;
        if (tRight && facing == 1) turned = 0;
        if (tLeft && facing == 3) turned = 0;
        if (turned == 1) {
            globalSet(ctx, maincharaCache.gFacing, RValue_makeReal(2.0));
            facing = 2;
        }
        Instance_setSelfVar(inst, maincharaCache.turned, RValue_makeReal((GMLReal)turned));
    }

    
    if (tRight && movement == 1 && !tLeft) {
        int32_t turned = 1;
        if (inst->xprevious == (inst->x - 3.0f)) inst->x += 2.0f;
        else inst->x += 3.0f;
        if (debugOn && kbCheck) {
            RValue a = RValue_makeReal(8.0);
            RValue r = kbCheck(ctx, &a, 1);
            if (RValue_toReal(r) != 0) inst->x += 5.0f;
            RValue_free(&r);
        }
        Instance_setSelfVar(inst, maincharaCache.moving, RValue_makeReal(1.0));
        inst->imageSpeed = 0.2f;
        
        
        
        if (tUp && facing == 2) turned = 0;
        if (tDown && facing == 0) turned = 0;
        if (turned == 1) {
            globalSet(ctx, maincharaCache.gFacing, RValue_makeReal(1.0));
            facing = 1;
        }
        Instance_setSelfVar(inst, maincharaCache.turned, RValue_makeReal((GMLReal)turned));
    }

    
    if (tDown && movement == 1 && !tUp) {
        int32_t turned = 1;
        inst->y += 3.0f;
        if (debugOn && kbCheck) {
            RValue a = RValue_makeReal(8.0);
            RValue r = kbCheck(ctx, &a, 1);
            if (RValue_toReal(r) != 0) inst->y += 5.0f;
            RValue_free(&r);
        }
        int32_t moving = selfInt(inst, maincharaCache.moving);
        if (moving != 1) inst->imageIndex = 1.0f;
        Instance_setSelfVar(inst, maincharaCache.moving, RValue_makeReal(1.0));
        inst->imageSpeed = 0.2f;
        if (tRight && facing == 1) turned = 0;
        if (tLeft && facing == 3) turned = 0;
        if (turned == 1) {
            globalSet(ctx, maincharaCache.gFacing, RValue_makeReal(0.0));
            facing = 0;
        }
        Instance_setSelfVar(inst, maincharaCache.turned, RValue_makeReal((GMLReal)turned));
    }

    
    if (mainchara_controlPressed(ctx, 0)) {
        Runner_executeEvent(runner, inst, 7, OTHER_USER0 + 0);
    }
    
    if (mainchara_controlPressed(ctx, 2)) {
        Runner_executeEvent(runner, inst, 7, OTHER_USER0 + 2);
    }

    
    
    InstanceBBox bboxAfterMove = Collision_computeBBox(ctx->dataWin, inst);
    if (bboxAfterMove.valid) {
        mainchara_fireCollisionRectEvent(ctx, runner, inst, &bboxAfterMove, maincharaCache.objTrigger, 9);
    }

    
    Instance* writerInst = findInstanceByObject(runner, maincharaCache.objWriter);
    if (writerInst == NULL) {
        
        DataWin* dw = ctx->dataWin;
        float sprH = 0;
        if (inst->spriteIndex >= 0 && (uint32_t)inst->spriteIndex < dw->sprt.count) {
            Sprite* spr = &dw->sprt.sprites[inst->spriteIndex];
            sprH = (float)spr->height * inst->imageYscale;
        }
        inst->depth = (int32_t)(50000.0f - (inst->y * 10.0f + sprH * 10.0f));

        
        if (maincharaCache.gFlag >= 0) {
            int64_t k = ((int64_t)maincharaCache.gFlag << 32) | 85u;
            ptrdiff_t i = hmgeti(ctx->globalArrayMap, k);
            if (i >= 0 && RValue_toReal(ctx->globalArrayMap[i].value) == 1.0) {
                int32_t dsprite = selfInt(inst, maincharaCache.dsprite);
                if (dsprite == 1104) {
                    inst->depth = (int32_t)(50000.0f - (inst->y * 10.0f + 300.0f));
                }
            }
        }
    }
}







static struct {
    int32_t radchange, specialtimer, radius, idealradius;
    int32_t nxadd, xadd, yadd;
    int32_t blue, cl;
    int32_t gIdealborder, gTurntimer, gMnfight;
    bool ready;
} loopblgCache = { .ready = false };

static void initLoopblgCache(VMContext* ctx, DataWin* dw) {
    loopblgCache.radchange = findSelfVarId(dw, "radchange");
    loopblgCache.specialtimer = findSelfVarId(dw, "specialtimer");
    loopblgCache.radius = findSelfVarId(dw, "radius");
    loopblgCache.idealradius = findSelfVarId(dw, "idealradius");
    loopblgCache.nxadd = findSelfVarId(dw, "nxadd");
    loopblgCache.xadd = findSelfVarId(dw, "xadd");
    loopblgCache.yadd = findSelfVarId(dw, "yadd");
    loopblgCache.blue = findSelfVarId(dw, "blue");
    loopblgCache.cl = findSelfVarId(dw, "cl");
    loopblgCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    loopblgCache.gTurntimer = findGlobalVarId(ctx, "turntimer");
    loopblgCache.gMnfight = findGlobalVarId(ctx, "mnfight");
    loopblgCache.ready = (loopblgCache.radius >= 0 && loopblgCache.specialtimer >= 0 &&
                          loopblgCache.idealradius >= 0 && loopblgCache.nxadd >= 0 &&
                          loopblgCache.gIdealborder >= 0 && loopblgCache.gTurntimer >= 0);
}

static void native_loopblg_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    if (!loopblgCache.ready) return;

    int32_t radchange = 0;
    GMLReal specialtimer = selfReal(inst, loopblgCache.specialtimer);
    GMLReal radius = selfReal(inst, loopblgCache.radius);
    GMLReal idealradius = selfReal(inst, loopblgCache.idealradius);

    if (specialtimer > 0) {
        if (radius < idealradius) { radius += 2.0; radchange = 1; }
        if (radius > idealradius) radius = idealradius;
    } else {
        if (radius > idealradius) { radius -= 2.0; radchange = 2; }
        if (radius < idealradius) radius = idealradius;
    }

    
    
    if (radchange != 0) {
        float len = (radchange == 1) ? 2.0f : -2.0f;
        double ang = (inst->direction - 90.0) * (M_PI / 180.0);
        inst->x += (float)(len * cos(ang));
        inst->y += (float)(len * -sin(ang));
    }

    
    
    double anglechange = 0;
    if (radius > 0) {
        double circ = 2.0 * M_PI * radius;
        anglechange = (circ > 0) ? (360.0 / (circ / inst->speed)) : 0;
    }
    inst->direction += (float)anglechange;
    
    inst->direction = fmodf(inst->direction, 360.0f);
    if (inst->direction < 0) inst->direction += 360.0f;
    Instance_computeComponentsFromSpeed(inst);

    GMLReal nxadd = selfReal(inst, loopblgCache.nxadd);
    GMLReal xadd = selfReal(inst, loopblgCache.xadd);
    GMLReal yadd = selfReal(inst, loopblgCache.yadd);
    if (nxadd < xadd) nxadd += 0.125;
    inst->x += (float)nxadd;
    inst->y += (float)yadd;

    specialtimer -= 1.0;
    if (specialtimer < 1.0) idealradius = 0.1;

    
    Instance_setSelfVar(inst, loopblgCache.radchange, RValue_makeReal((GMLReal)radchange));
    Instance_setSelfVar(inst, loopblgCache.specialtimer, RValue_makeReal(specialtimer));
    Instance_setSelfVar(inst, loopblgCache.radius, RValue_makeReal(radius));
    Instance_setSelfVar(inst, loopblgCache.idealradius, RValue_makeReal(idealradius));
    Instance_setSelfVar(inst, loopblgCache.nxadd, RValue_makeReal(nxadd));
}

static void native_loopblg_Step2(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!loopblgCache.ready) return;
    
    int64_t k1 = ((int64_t)loopblgCache.gIdealborder << 32) | 1u;
    ptrdiff_t i1 = hmgeti(ctx->globalArrayMap, k1);
    if (i1 >= 0) {
        GMLReal border1 = RValue_toReal(ctx->globalArrayMap[i1].value);
        if (inst->x >= border1) { Runner_destroyInstance(runner, inst); return; }
    }
    
    if (RValue_toReal(ctx->globalVars[loopblgCache.gTurntimer]) < 1.0) {
        Runner_destroyInstance(runner, inst);
    }
}


static void native_loopblg_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!loopblgCache.ready) return;

    
    if (selfInt(inst, loopblgCache.blue) == 1 && inst->spriteIndex != 171) {
        inst->spriteIndex = 171;
    }

    DataWin* dw = ctx->dataWin;
    Renderer* r = runner->renderer;
    if (!r) return;

    int32_t cl = selfInt(inst, loopblgCache.cl);
    if (cl != 0) {
        
        Renderer_drawSprite(r, inst->spriteIndex, (int32_t)inst->imageIndex, inst->x, inst->y);
        return;
    }

    
    if (!chasefire2Cache.ready) { Renderer_drawSelf(r, inst); return; }
    if (inst->spriteIndex < 0 || (uint32_t)inst->spriteIndex >= dw->sprt.count) return;

    Sprite* spr = &dw->sprt.sprites[inst->spriteIndex];
    float sw = (float)spr->width * inst->imageXscale;
    float sh = (float)spr->height * inst->imageYscale;
    const float offx = 8.0f;
    const float offy = 8.0f;

    updateBorderPosCache(ctx, runner);
    float lbx = borderPosCache.lbx;
    float rbx = borderPosCache.rbx;
    float border2 = borderPosCache.border2;
    float border3 = borderPosCache.border3;

    float l = 0, t = 0, w = sw, h = sh;
    float ll = ((lbx - inst->x) + 1.0f) - offx;
    float tt = ((border2 - inst->y) + 1.0f) - offy;
    float ww = (inst->x + w) - rbx - 1.0f;
    float hh = (inst->y + h) - border3 - 1.0f;

    if (ll > 0) l += ll;
    if (tt > 0) t += tt;
    if (ww > 0) w -= ww;
    if (hh > 0) h -= hh;

    w = GMLReal_round(w); h = GMLReal_round(h);
    l = GMLReal_round(l); t = GMLReal_round(t);

    if ((w + offx) > 0 && (h + offy) > 0 && l < w && t < h) {
        int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, inst->spriteIndex, (int32_t)inst->imageIndex);
        if (tpagIndex >= 0) {
            
            r->vtable->drawSpritePart(r, tpagIndex,
                (int32_t)l, (int32_t)t,
                (int32_t)((w - l) + offx), (int32_t)((h - t) + offy),
                (inst->x + l) - offx, (inst->y + t) - offy,
                1.0f, 1.0f, 0xFFFFFF, inst->imageAlpha);
        }
    }
}








static struct {
    int32_t touched;
    int32_t fvic, vic;
    int32_t objXoxoctrl;
    int32_t objTrigger;     
    int32_t obj978;         
    bool ready;
    
    uint64_t frame;
    Instance* trigger;
    InstanceBBox triggerBBox;
    bool triggerValid;
    Instance* inst978;
    Instance* xoxoctrlInst;
} xoxoCache = { .ready = false, .frame = UINT64_MAX };

static void initXoxoCache(VMContext* ctx, DataWin* dw) {
    (void)ctx;
    xoxoCache.touched = findSelfVarId(dw, "touched");
    xoxoCache.fvic = findSelfVarId(dw, "fvic");
    xoxoCache.vic = findSelfVarId(dw, "vic");
    xoxoCache.objXoxoctrl = findObjectIndex(dw, "obj_xoxocontroller1");
    xoxoCache.objTrigger = 1576;
    xoxoCache.obj978 = 978;
    xoxoCache.ready = (xoxoCache.touched >= 0 && xoxoCache.fvic >= 0 && xoxoCache.vic >= 0);
}

static void refreshXoxoFrameCache(Runner* runner) {
    uint64_t frame = (uint64_t)runner->frameCount;
    if (xoxoCache.frame == frame) return;
    xoxoCache.frame = frame;
    xoxoCache.trigger = findInstanceByObject(runner, xoxoCache.objTrigger);
    xoxoCache.triggerValid = false;
    if (xoxoCache.trigger) {
        xoxoCache.triggerBBox = Collision_computeBBox(runner->dataWin, xoxoCache.trigger);
        xoxoCache.triggerValid = xoxoCache.triggerBBox.valid;
    }
    xoxoCache.inst978 = findInstanceByObject(runner, xoxoCache.obj978);
    xoxoCache.xoxoctrlInst = (xoxoCache.objXoxoctrl >= 0)
                             ? findInstanceByObject(runner, xoxoCache.objXoxoctrl) : NULL;
}

static void native_xoxo_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!xoxoCache.ready) return;
    refreshXoxoFrameCache(runner);

    InstanceBBox selfBBox = Collision_computeBBox(ctx->dataWin, inst);
    if (!selfBBox.valid) return;

    float checkX = inst->x + 10.0f;
    float y1 = (float)selfBBox.top - 2.0f;
    float y2 = (float)selfBBox.bottom - 2.0f;

    
    bool collision = false;
    if (xoxoCache.triggerValid && xoxoCache.trigger != inst) {
        InstanceBBox tb = xoxoCache.triggerBBox;
        
        if (!(checkX >= tb.right || tb.left >= checkX || y1 >= tb.bottom || tb.top >= y2)) {
            collision = true;
        }
    }

    int32_t touched = selfInt(inst, xoxoCache.touched);
    int32_t imageIdx = (int32_t)inst->imageIndex;

    if (collision && touched == 0) {
        Instance_setSelfVar(inst, xoxoCache.touched, RValue_makeReal(1.0));
        touched = 1;

        
        
        if (imageIdx == 1) {
            BuiltinFunc aps = VMBuiltins_find("audio_play_sound");
            if (aps) {
                RValue a[3] = { RValue_makeReal(142), RValue_makeReal(80), RValue_makeReal(0) };
                RValue r = aps(ctx, a, 3); RValue_free(&r);
            }
            inst->imageIndex = 2.0f;
            imageIdx = 2;
            if (xoxoCache.xoxoctrlInst) {
                Instance_setSelfVar(xoxoCache.xoxoctrlInst, xoxoCache.fvic, RValue_makeReal(0.0));
            }
        } else if (imageIdx == 0) {
            inst->imageIndex = 1.0f;
            imageIdx = 1;
            BuiltinFunc aps = VMBuiltins_find("audio_play_sound");
            if (aps) {
                RValue a[3] = { RValue_makeReal(142), RValue_makeReal(80), RValue_makeReal(0) };
                RValue r = aps(ctx, a, 3); RValue_free(&r);
            }
        }
    }

    
    if (!collision && touched == 1) {
        Instance_setSelfVar(inst, xoxoCache.touched, RValue_makeReal(0.0));
    }

    
    if (xoxoCache.inst978 != NULL && imageIdx == 1 && xoxoCache.xoxoctrlInst) {
        GMLReal vic = selfReal(xoxoCache.xoxoctrlInst, xoxoCache.vic);
        Instance_setSelfVar(xoxoCache.xoxoctrlInst, xoxoCache.vic, RValue_makeReal(vic + 1.0));
    }
}







static struct {
    int32_t mygrey, gg, garfield, rando, randofactor, finalrando, kingrando;
    int32_t computersound; 
    int32_t objPapyrus4;
    bool ready;
    
    uint64_t frame;
    Instance* papyrus4;
} specialtileCache = { .ready = false, .frame = UINT64_MAX };

static void initSpecialtileCache(VMContext* ctx, DataWin* dw) {
    (void)ctx;
    specialtileCache.mygrey = findSelfVarId(dw, "mygrey");
    specialtileCache.gg = findSelfVarId(dw, "gg");
    specialtileCache.garfield = findSelfVarId(dw, "garfield");
    specialtileCache.rando = findSelfVarId(dw, "rando");
    specialtileCache.randofactor = findSelfVarId(dw, "randofactor");
    specialtileCache.finalrando = findSelfVarId(dw, "finalrando");
    specialtileCache.kingrando = findSelfVarId(dw, "kingrando");
    specialtileCache.computersound = findSelfVarId(dw, "computersound");
    specialtileCache.objPapyrus4 = findObjectIndex(dw, "obj_papyrus4");
    specialtileCache.ready = (specialtileCache.mygrey >= 0 && specialtileCache.rando >= 0 &&
                              specialtileCache.garfield >= 0 && specialtileCache.randofactor >= 0);
}

static void native_specialtile_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!specialtileCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;
    uint32_t color = (uint32_t)selfInt(inst, specialtileCache.mygrey);
    r->drawColor = color;
    r->vtable->drawRectangle(r, inst->x, inst->y, inst->x + 19.0f, inst->y + 19.0f,
                              color, r->drawAlpha, false);
}

static void native_specialtile_Alarm0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!specialtileCache.ready) return;

    
    if (selfInt(inst, specialtileCache.rando) != 0) return;

    GMLReal garfield = selfReal(inst, specialtileCache.garfield);
    GMLReal randofactor = selfReal(inst, specialtileCache.randofactor);
    GMLReal finalrando = selfReal(inst, specialtileCache.finalrando);
    int32_t kingrando = selfInt(inst, specialtileCache.kingrando);

    garfield /= 1.02;
    randofactor /= 1.1;

    if (randofactor < 3.0) {
        randofactor = 3.0;
        finalrando += 1.0;
    }
    if (finalrando > 12.0) randofactor = 2.0;
    if (finalrando > 30.0) randofactor = 1.0;

    garfield -= 1.0;

    if (finalrando > 120.0) randofactor = -1.0;

    if (kingrando == 1) {
        
        uint64_t frame = (uint64_t)runner->frameCount;
        if (specialtileCache.frame != frame) {
            specialtileCache.frame = frame;
            specialtileCache.papyrus4 = (specialtileCache.objPapyrus4 >= 0)
                ? findInstanceByObject(runner, specialtileCache.objPapyrus4) : NULL;
        }
        if (specialtileCache.papyrus4 && specialtileCache.computersound >= 0) {
            GMLReal sound = selfReal(specialtileCache.papyrus4, specialtileCache.computersound);
            GMLReal pitch = 3.0 / ((garfield / 20.0) + 2.5);
            BuiltinFunc asp = VMBuiltins_find("audio_sound_pitch");
            if (asp) {
                RValue a[2] = { RValue_makeReal(sound), RValue_makeReal(pitch) };
                RValue r = asp(ctx, a, 2); RValue_free(&r);
            }
        }
    }

    
    inst->alarm[0] = (int32_t)randofactor;

    
    int32_t gg = (int32_t)(((double)rand() / ((double)RAND_MAX + 1.0)) * 7.0);
    if (gg > 6) gg = 6;
    if (gg < 0) gg = 0;

    int32_t mygrey = 0;
    
    switch (gg) {
        case 0: mygrey = 16711680; break; 
        case 1: mygrey = 65535;    break; 
        case 2: mygrey = 65280;    break; 
        case 3: mygrey = 8388736;  break; 
        case 4: mygrey = 4235519;  break; 
        case 5: mygrey = 255;      break; 
        case 6: mygrey = (255) | (100 << 8) | (100 << 16); break; 
        default: break;
    }

    if (randofactor == -1.0) {
        kingrando = 0;
        
        if (inst->y < 120.0f || inst->y >= 160.0f) mygrey = 255;
        else mygrey = (255) | (100 << 8) | (100 << 16);
        Instance_setSelfVar(inst, specialtileCache.kingrando, RValue_makeReal(0.0));
    }

    
    Instance_setSelfVar(inst, specialtileCache.garfield, RValue_makeReal(garfield));
    Instance_setSelfVar(inst, specialtileCache.randofactor, RValue_makeReal(randofactor));
    Instance_setSelfVar(inst, specialtileCache.finalrando, RValue_makeReal(finalrando));
    Instance_setSelfVar(inst, specialtileCache.gg, RValue_makeReal((GMLReal)gg));
    Instance_setSelfVar(inst, specialtileCache.mygrey, RValue_makeReal((GMLReal)mygrey));
}







static struct {
    int32_t writer, xx, side, count;
    int32_t writer_writingy;
    int32_t writer_halt, writer_originalstring, writer_stringpos;
    int32_t face_y;  
    int32_t gFacechange, gFacechoice, gTyper, gFlag, gLanguage;
    int32_t objFace, objWriter;  
    int32_t dialoguerCodeStepId; 
    bool ready;
} dialoguerCache = { .ready = false };

static void initDialoguerCache(VMContext* ctx, DataWin* dw) {
    dialoguerCache.writer = findSelfVarId(dw, "writer");
    dialoguerCache.xx = findSelfVarId(dw, "xx");
    dialoguerCache.side = findSelfVarId(dw, "side");
    dialoguerCache.count = findSelfVarId(dw, "count");
    
    dialoguerCache.writer_writingy = findSelfVarId(dw, "writingy");
    dialoguerCache.writer_halt = findSelfVarId(dw, "halt");
    dialoguerCache.writer_originalstring = findSelfVarId(dw, "originalstring");
    dialoguerCache.writer_stringpos = findSelfVarId(dw, "stringpos");
    dialoguerCache.gFacechange = findGlobalVarId(ctx, "facechange");
    dialoguerCache.gFacechoice = findGlobalVarId(ctx, "facechoice");
    dialoguerCache.gTyper = findGlobalVarId(ctx, "typer");
    dialoguerCache.gFlag = findGlobalVarId(ctx, "flag");
    dialoguerCache.gLanguage = findGlobalVarId(ctx, "language");
    dialoguerCache.objFace = findObjectIndex(dw, "obj_face");
    dialoguerCache.objWriter = findObjectIndex(dw, "obj_base_writer");
    dialoguerCache.dialoguerCodeStepId = -1;
    for (uint32_t ci = 0; ci < dw->code.count; ci++) {
        if (strcmp(dw->code.entries[ci].name, "gml_Object_obj_dialoguer_Step_0") == 0) {
            dialoguerCache.dialoguerCodeStepId = (int32_t)ci; break;
        }
    }
    dialoguerCache.ready = (dialoguerCache.writer >= 0 && dialoguerCache.gFacechange >= 0);
}


static Instance* findInstanceByInstId(Runner* runner, uint32_t id) {
    ptrdiff_t idx = hmgeti(runner->instancesToId, id);
    if (idx < 0) return NULL;
    return runner->instancesToId[idx].value;
}

static void native_dialoguer_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!dialoguerCache.ready) return;

    
    
    
    GMLReal facechange = (dialoguerCache.gFacechange >= 0) ? globalReal(ctx, dialoguerCache.gFacechange) : 0;
    if ((int32_t)facechange != 0) {
        if (dialoguerCache.dialoguerCodeStepId >= 0) {
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue r = VM_executeCode(ctx, dialoguerCache.dialoguerCodeStepId);
            RValue_free(&r);
            ctx->currentInstance = saved;
        }
        return;
    }

    
    uint32_t writerId = (uint32_t)selfInt(inst, dialoguerCache.writer);
    Instance* writerInst = findInstanceByInstId(runner, writerId);

    
    if (writerInst == NULL || !writerInst->active) {
        Runner_destroyInstance(runner, inst);
        return;
    }

    
    static int32_t gCP = -2;
    if (gCP == -2) gCP = findGlobalVarId(ctx, "control_pressed");
    if (gCP < 0) return;
    int64_t k = ((int64_t)gCP << 32) | 1u;
    ptrdiff_t i = hmgeti(ctx->globalArrayMap, k);
    if (i < 0) return;
    if (RValue_toReal(ctx->globalArrayMap[i].value) == 0) return;

    
    if (dialoguerCache.writer_halt >= 0) {
        int32_t halt = selfInt(writerInst, dialoguerCache.writer_halt);
        if (halt == 0) {
            GMLReal typer = (dialoguerCache.gTyper >= 0) ? globalReal(ctx, dialoguerCache.gTyper) : 0;
            if ((int32_t)typer != 10) {
                
                if (dialoguerCache.gFlag >= 0) {
                    int64_t fk = ((int64_t)dialoguerCache.gFlag << 32) | 25u;
                    ptrdiff_t fi = hmgeti(ctx->globalArrayMap, fk);
                    GMLReal cur = (fi >= 0) ? RValue_toReal(ctx->globalArrayMap[fi].value) : 0;
                    globalArraySet(ctx, dialoguerCache.gFlag, 25, RValue_makeReal(cur + 1.0));
                }
                
                const char* s = selfString(writerInst, dialoguerCache.writer_originalstring);
                int32_t len = (int32_t)strlen(s); 
                Instance_setSelfVar(writerInst, dialoguerCache.writer_stringpos, RValue_makeReal((GMLReal)len));
            }
        }
    }
    
    globalArraySet(ctx, gCP, 1, RValue_makeReal(0.0));
}

static void native_dialoguer_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!dialoguerCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r || !runner->currentRoom) return;

    int32_t side = selfInt(inst, dialoguerCache.side);
    float vx = (float)runner->currentRoom->views[runner->viewCurrent].viewX;
    float vy = (float)runner->currentRoom->views[runner->viewCurrent].viewY;

    
    uint32_t writerId = (uint32_t)selfInt(inst, dialoguerCache.writer);
    Instance* writerInst = findInstanceByInstId(runner, writerId);
    if (writerInst && writerInst->active && dialoguerCache.writer_writingy >= 0) {
        GMLReal wy = selfReal(writerInst, dialoguerCache.writer_writingy);
        if (side == 0) {
            if (wy > (vy + 80.0f)) Instance_setSelfVar(writerInst, dialoguerCache.writer_writingy, RValue_makeReal(wy - 155.0));
        } else {
            if (wy < (vy + 80.0f)) Instance_setSelfVar(writerInst, dialoguerCache.writer_writingy, RValue_makeReal(wy + 155.0));
        }
    }

    
    
    
    
    if (dialoguerCache.objFace >= 0 && dialoguerCache.objFace < runner->instancesByObjMax &&
        runner->instancesByObjInclParent != NULL) {
        Instance** list = runner->instancesByObjInclParent[dialoguerCache.objFace];
        int32_t n = (int32_t)arrlen(list);
        Instance* face = NULL;
        for (int32_t i = 0; i < n; i++) {
            if (list[i]->active) { face = list[i]; break; }
        }
        if (face) {
            if (side == 0) { if (face->y > (vy + 80.0f)) face->y -= 155.0f; }
            else           { if (face->y < (vy + 80.0f)) face->y += 155.0f; }
        }
    }

    
    
    float y1Outer, y2Outer, y1Inner, y2Inner;
    if (side == 0) {
        y1Outer = vy + 5.0f;  y2Outer = vy + 80.0f;
        y1Inner = vy + 8.0f;  y2Inner = vy + 77.0f;
    } else {
        y1Outer = vy + 160.0f; y2Outer = vy + 235.0f;
        y1Inner = vy + 163.0f; y2Inner = vy + 232.0f;
    }
    r->drawColor = 0xFFFFFF;
    r->vtable->drawRectangle(r, vx + 16.0f, y1Outer, vx + 304.0f, y2Outer, 0xFFFFFF, r->drawAlpha, false);
    r->drawColor = 0;
    r->vtable->drawRectangle(r, vx + 19.0f, y1Inner, vx + 301.0f, y2Inner, 0, r->drawAlpha, false);

    
    
    
    if (dialoguerCache.count >= 0) {
        Instance_setSelfVar(inst, dialoguerCache.count, RValue_makeReal(1.0));
    }
}








static struct {
    
    int32_t buffer, currentmenu, currentspot, xx_s, yy_s, moveyy_s, nextlevel;
    
    int32_t gInteract, gMenuno, gMenucoord, gMenuchoice;
    int32_t gLanguage, gLv, gHp, gMaxhp, gGold, gCharname;
    int32_t gItem, gItemname, gPhone, gPhonename;
    int32_t gFlag, gAt, gDf, gWstrength, gAdef, gWeapon, gArmor, gXp, gKills;
    int32_t gControlPressed;
    
    int32_t objMainchara;
    
    int32_t scrItemname, scrStoragename, scrPhonename;
    int32_t scrItemuseb, scrItemdesc, scrWritetext, scrItemshift;
    int32_t scrStorageget, scrStorageshift, scrItemget;
    
    int32_t codeId;
    bool ready;
} ovrctrlCache = { .ready = false };

static void initOvrctrlCache(VMContext* ctx, DataWin* dw) {
    ovrctrlCache.buffer = findSelfVarId(dw, "buffer");
    ovrctrlCache.currentmenu = findSelfVarId(dw, "currentmenu");
    ovrctrlCache.currentspot = findSelfVarId(dw, "currentspot");
    ovrctrlCache.xx_s = findSelfVarId(dw, "xx");
    ovrctrlCache.yy_s = findSelfVarId(dw, "yy");
    ovrctrlCache.moveyy_s = findSelfVarId(dw, "moveyy");
    ovrctrlCache.nextlevel = findSelfVarId(dw, "nextlevel");

    ovrctrlCache.gInteract = findGlobalVarId(ctx, "interact");
    ovrctrlCache.gMenuno = findGlobalVarId(ctx, "menuno");
    ovrctrlCache.gMenucoord = findGlobalVarId(ctx, "menucoord");
    ovrctrlCache.gMenuchoice = findGlobalVarId(ctx, "menuchoice");
    ovrctrlCache.gLanguage = findGlobalVarId(ctx, "language");
    ovrctrlCache.gLv = findGlobalVarId(ctx, "lv");
    ovrctrlCache.gHp = findGlobalVarId(ctx, "hp");
    ovrctrlCache.gMaxhp = findGlobalVarId(ctx, "maxhp");
    ovrctrlCache.gGold = findGlobalVarId(ctx, "gold");
    ovrctrlCache.gCharname = findGlobalVarId(ctx, "charname");
    ovrctrlCache.gItem = findGlobalVarId(ctx, "item");
    ovrctrlCache.gItemname = findGlobalVarId(ctx, "itemname");
    ovrctrlCache.gPhone = findGlobalVarId(ctx, "phone");
    ovrctrlCache.gPhonename = findGlobalVarId(ctx, "phonename");
    ovrctrlCache.gFlag = findGlobalVarId(ctx, "flag");
    ovrctrlCache.gAt = findGlobalVarId(ctx, "at");
    ovrctrlCache.gDf = findGlobalVarId(ctx, "df");
    ovrctrlCache.gWstrength = findGlobalVarId(ctx, "wstrength");
    ovrctrlCache.gAdef = findGlobalVarId(ctx, "adef");
    ovrctrlCache.gWeapon = findGlobalVarId(ctx, "weapon");
    ovrctrlCache.gArmor = findGlobalVarId(ctx, "armor");
    ovrctrlCache.gXp = findGlobalVarId(ctx, "xp");
    ovrctrlCache.gKills = findGlobalVarId(ctx, "kills");
    ovrctrlCache.gControlPressed = findGlobalVarId(ctx, "control_pressed");
    ovrctrlCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    ovrctrlCache.scrItemname = findScriptCodeId(ctx, "scr_itemname");
    ovrctrlCache.scrStoragename = findScriptCodeId(ctx, "scr_storagename");
    ovrctrlCache.scrPhonename = findScriptCodeId(ctx, "scr_phonename");
    ovrctrlCache.scrItemuseb = findScriptCodeId(ctx, "scr_itemuseb");
    ovrctrlCache.scrItemdesc = findScriptCodeId(ctx, "scr_itemdesc");
    ovrctrlCache.scrWritetext = findScriptCodeId(ctx, "scr_writetext");
    ovrctrlCache.scrItemshift = findScriptCodeId(ctx, "scr_itemshift");
    ovrctrlCache.scrStorageget = findScriptCodeId(ctx, "scr_storageget");
    ovrctrlCache.scrStorageshift = findScriptCodeId(ctx, "scr_storageshift");
    ovrctrlCache.scrItemget = findScriptCodeId(ctx, "scr_itemget");
    ovrctrlCache.codeId = -1;
    for (uint32_t ci = 0; ci < dw->code.count; ci++) {
        if (strcmp(dw->code.entries[ci].name, "gml_Object_obj_overworldcontroller_Draw_0") == 0) {
            ovrctrlCache.codeId = (int32_t)ci; break;
        }
    }
    ovrctrlCache.ready = (ovrctrlCache.buffer >= 0 && ovrctrlCache.gInteract >= 0 &&
                          ovrctrlCache.gMenuno >= 0 && ovrctrlCache.gMenucoord >= 0);
}


static inline bool ovrctrl_controlPressed(VMContext* ctx, int32_t idx) {
    if (ovrctrlCache.gControlPressed < 0 || idx < 0 || idx > 2) return false;
    int64_t k = ((int64_t)ovrctrlCache.gControlPressed << 32) | (uint32_t)idx;
    ptrdiff_t i = hmgeti(ctx->globalArrayMap, k);
    if (i < 0) return false;
    return RValue_toReal(ctx->globalArrayMap[i].value) != 0;
}
static inline void ovrctrl_controlClear(VMContext* ctx, int32_t idx) {
    if (ovrctrlCache.gControlPressed < 0) return;
    globalArraySet(ctx, ovrctrlCache.gControlPressed, idx, RValue_makeReal(0.0));
}
static inline const char* ovrctrl_getText(VMContext* ctx, const char* key) {
    static RValue cached[1];
    (void)cached;
    BuiltinFunc gt = VMBuiltins_find("scr_gettext");
    if (!gt) return "";
    RValue arg = RValue_makeString((char*)key);
    RValue r = gt(ctx, &arg, 1);
    static char buf[512];
    if (r.type == RVALUE_STRING && r.string) {
        strncpy(buf, r.string, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
    } else {
        buf[0] = '\0';
    }
    RValue_free(&r);
    return buf;
}

static inline void ovrctrl_getTextArg(VMContext* ctx, const char* key, const char* arg1, char* out, size_t outSize) {
    BuiltinFunc gt = VMBuiltins_find("scr_gettext");
    if (!gt) { out[0] = '\0'; return; }
    RValue args[2] = { RValue_makeString((char*)key), RValue_makeString((char*)arg1) };
    RValue r = gt(ctx, args, 2);
    if (r.type == RVALUE_STRING && r.string) {
        strncpy(out, r.string, outSize - 1);
        out[outSize - 1] = '\0';
    } else {
        out[0] = '\0';
    }
    RValue_free(&r);
}
static inline void ovrctrl_getTextArg2(VMContext* ctx, const char* key, const char* a1, const char* a2, char* out, size_t outSize) {
    BuiltinFunc gt = VMBuiltins_find("scr_gettext");
    if (!gt) { out[0] = '\0'; return; }
    RValue args[3] = { RValue_makeString((char*)key), RValue_makeString((char*)a1), RValue_makeString((char*)a2) };
    RValue r = gt(ctx, args, 3);
    if (r.type == RVALUE_STRING && r.string) {
        strncpy(out, r.string, outSize - 1);
        out[outSize - 1] = '\0';
    } else {
        out[0] = '\0';
    }
    RValue_free(&r);
}

static void native_overworldctrl_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!ovrctrlCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r || !runner->currentRoom) return;

    
    
    
    
    int32_t menuno = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
    int32_t interact = (int32_t)globalReal(ctx, ovrctrlCache.gInteract);
    bool fallbackVM = false;
    if (interact == 5) {
        if (menuno == 5 && ovrctrl_controlPressed(ctx, 0) &&
            (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 5) == 2) fallbackVM = true;
        else if ((menuno == 3 || menuno == 6 || menuno == 7) && ovrctrl_controlPressed(ctx, 0)) {
            fallbackVM = true;
        }
        else if (menuno == 4 && ovrctrl_controlPressed(ctx, 0) &&
                 (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 4) == 0) {
            
            fallbackVM = true;
        }
    }
    if (fallbackVM) {
        if (ovrctrlCache.codeId >= 0) {
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue rr = VM_executeCode(ctx, ovrctrlCache.codeId);
            RValue_free(&rr);
            ctx->currentInstance = saved;
        }
        return;
    }

    
    GMLReal buffer = selfReal(inst, ovrctrlCache.buffer) + 1.0;
    Instance_setSelfVar(inst, ovrctrlCache.buffer, RValue_makeReal(buffer));

    if (interact == 5) {
        int32_t currentmenu = menuno;
        GMLReal currentspot = 0;
        if (menuno < 6 && menuno >= 0) {
            currentspot = getGlobalArray(ctx, ovrctrlCache.gMenucoord, menuno);
        }
        Instance_setSelfVar(inst, ovrctrlCache.currentmenu, RValue_makeReal((GMLReal)currentmenu));
        Instance_setSelfVar(inst, ovrctrlCache.currentspot, RValue_makeReal(currentspot));

        float xx = (float)runner->currentRoom->views[runner->viewCurrent].viewX;
        float yy = (float)runner->currentRoom->views[runner->viewCurrent].viewY + 10.0f;
        float moveyy = yy;
        Instance* mainchara = (ovrctrlCache.objMainchara >= 0) ? findInstanceByObject(runner, ovrctrlCache.objMainchara) : NULL;
        if (mainchara && mainchara->y > (yy + 120.0f)) moveyy += 135.0f;
        Instance_setSelfVar(inst, ovrctrlCache.xx_s, RValue_makeReal((GMLReal)xx));
        Instance_setSelfVar(inst, ovrctrlCache.yy_s, RValue_makeReal((GMLReal)yy));
        Instance_setSelfVar(inst, ovrctrlCache.moveyy_s, RValue_makeReal((GMLReal)moveyy));

        const char* lang = globalString(ctx, ovrctrlCache.gLanguage);
        bool isJa = (lang && strcmp(lang, "ja") == 0);

        
        
        if (menuno != 4) {
            
            r->drawColor = 0xFFFFFF;
            r->vtable->drawRectangle(r, 16.0f + xx, 16.0f + moveyy, 86.0f + xx, 70.0f + moveyy, 0xFFFFFF, r->drawAlpha, false);
            r->vtable->drawRectangle(r, 16.0f + xx, 74.0f + yy, 86.0f + xx, 147.0f + yy, 0xFFFFFF, r->drawAlpha, false);

            if (menuno == 1 || menuno == 5 || menuno == 6) {
                r->vtable->drawRectangle(r, 94.0f + xx, 16.0f + yy, 266.0f + xx, 196.0f + yy, 0xFFFFFF, r->drawAlpha, false);
            } else if (menuno == 2) {
                float xend = 266.0f; if (isJa) xend += 9.0f;
                r->vtable->drawRectangle(r, 94.0f + xx, 16.0f + yy, xend + xx, 224.0f + yy, 0xFFFFFF, r->drawAlpha, false);
            } else if (menuno == 3) {
                r->vtable->drawRectangle(r, 94.0f + xx, 16.0f + yy, 266.0f + xx, 150.0f + yy, 0xFFFFFF, r->drawAlpha, false);
            } else if (menuno == 7) {
                r->vtable->drawRectangle(r, 94.0f + xx, 16.0f + yy, 266.0f + xx, 216.0f + yy, 0xFFFFFF, r->drawAlpha, false);
            }

            r->drawColor = 0;
            r->vtable->drawRectangle(r, 19.0f + xx, 19.0f + moveyy, 83.0f + xx, 67.0f + moveyy, 0, r->drawAlpha, false);
            r->vtable->drawRectangle(r, 19.0f + xx, 77.0f + yy, 83.0f + xx, 144.0f + yy, 0, r->drawAlpha, false);
            if (menuno == 1 || menuno == 5 || menuno == 6) {
                r->vtable->drawRectangle(r, 97.0f + xx, 19.0f + yy, 263.0f + xx, 193.0f + yy, 0, r->drawAlpha, false);
            } else if (menuno == 2) {
                float xend = 263.0f; if (isJa) xend += 9.0f;
                r->vtable->drawRectangle(r, 97.0f + xx, 19.0f + yy, xend + xx, 221.0f + yy, 0, r->drawAlpha, false);
            } else if (menuno == 3) {
                r->vtable->drawRectangle(r, 97.0f + xx, 19.0f + yy, 263.0f + xx, 147.0f + yy, 0, r->drawAlpha, false);
            } else if (menuno == 7) {
                r->vtable->drawRectangle(r, 97.0f + xx, 19.0f + yy, 263.0f + xx, 213.0f + yy, 0, r->drawAlpha, false);
            }

            
            r->drawColor = 0xFFFFFF;
            r->drawFont = isJa ? 18 : 3;  
            float numpos = 23.0f + xx + nativeStringWidth(runner, r, "LV  ");
            char buf[64];
            snprintf(buf, sizeof(buf), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gLv));
            nativeDrawText(runner, r, 23.0f + xx, 40.0f + moveyy, "LV");
            nativeDrawText(runner, r, numpos, 40.0f + moveyy, buf);
            snprintf(buf, sizeof(buf), "%d/%d", (int32_t)globalReal(ctx, ovrctrlCache.gHp), (int32_t)globalReal(ctx, ovrctrlCache.gMaxhp));
            nativeDrawText(runner, r, 23.0f + xx, 49.0f + moveyy, "HP");
            nativeDrawText(runner, r, numpos, 49.0f + moveyy, buf);
            snprintf(buf, sizeof(buf), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gGold));
            nativeDrawText(runner, r, 23.0f + xx, 58.0f + moveyy, "G");
            nativeDrawText(runner, r, numpos, 58.0f + moveyy, buf);

            
            r->drawFont = isJa ? 14 : 2;
            float name0_y = 20.0f + moveyy;
            float name0_scale = 1.0f;
            if (isJa) { r->drawFont = 12; name0_y += 4.0f; name0_scale = 0.5f; }
            const char* charname = globalString(ctx, ovrctrlCache.gCharname);
            char* charnameProc = TextUtils_preprocessGmlTextIfNeeded(runner, charname);
            r->vtable->drawText(r, charnameProc, 23.0f + xx, name0_y, name0_scale, name0_scale, 0);
            free(charnameProc);

            r->drawFont = isJa ? 14 : 2;
            float xx0 = xx; if (isJa) xx0 -= 2.0f;

            
            if ((int32_t)getGlobalArray(ctx, ovrctrlCache.gItem, 0) == 0) r->drawColor = 0x808080;
            if ((int32_t)getGlobalArray(ctx, ovrctrlCache.gMenuchoice, 0) == 1) {
                nativeDrawText(runner, r, 42.0f + xx0, 84.0f + yy, ovrctrl_getText(ctx, "field_menu_item"));
            }
            r->drawColor = 0xFFFFFF;
            if ((int32_t)getGlobalArray(ctx, ovrctrlCache.gMenuchoice, 1) == 1) {
                nativeDrawText(runner, r, 42.0f + xx, 102.0f + yy, ovrctrl_getText(ctx, "field_menu_stat"));
            }
            if ((int32_t)getGlobalArray(ctx, ovrctrlCache.gMenuchoice, 2) == 1) {
                nativeDrawText(runner, r, 42.0f + xx, 120.0f + yy, ovrctrl_getText(ctx, "field_menu_cell"));
            }

            
            if (menuno == 1 || menuno == 5) {
                for (int32_t i = 0; i < 8; i++) {
                    const char* name = swapperGetArrayString(ctx, ovrctrlCache.gItemname, i);
                    nativeDrawText(runner, r, 116.0f + xx, 30.0f + yy + (float)(i * 16), name);
                }
                nativeDrawText(runner, r, 116.0f + xx,        170.0f + yy, ovrctrl_getText(ctx, "item_menu_use"));
                nativeDrawText(runner, r, 116.0f + xx + 48.f, 170.0f + yy, ovrctrl_getText(ctx, "item_menu_info"));
                nativeDrawText(runner, r, 116.0f + xx + 105.f, 170.0f + yy, ovrctrl_getText(ctx, "item_menu_drop"));
            }
        } 

        
        if (menuno == 3) {
            for (int32_t i = 0; i < 7; i++) {
                const char* name = swapperGetArrayString(ctx, ovrctrlCache.gPhonename, i);
                nativeDrawText(runner, r, 116.0f + xx, 30.0f + yy + (float)(i * 16), name);
            }
        }

        
        if (menuno == 6) {
            if (ovrctrlCache.scrItemname >= 0) {
                RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrItemname, NULL, 0);
                RValue_free(&rr);
            }
            for (int32_t i = 0; i < 8; i++) {
                const char* name = swapperGetArrayString(ctx, ovrctrlCache.gItemname, i);
                nativeDrawText(runner, r, 116.0f + xx, 30.0f + yy + (float)(i * 16), name);
            }
        }

        
        if (menuno == 7) {
            if (ovrctrlCache.scrStoragename >= 0) {
                RValue arg = RValue_makeReal(300.0);
                RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrStoragename, &arg, 1);
                RValue_free(&rr);
            }
            for (int32_t i = 0; i < 10; i++) {
                const char* name = swapperGetArrayString(ctx, ovrctrlCache.gItemname, i);
                nativeDrawText(runner, r, 116.0f + xx, 30.0f + yy + (float)(i * 16), name);
            }
        }

        
        if (menuno == 2) {
            float stat_x = 108.0f + xx;
            if (isJa) stat_x -= 3.0f;
            float exp_x = stat_x + 84.0f;
            float kills_x = exp_x;
            float name_y = 32.0f + yy, lv_y = 62.0f + yy, hp_y = 78.0f + yy;
            float at_y = 110.0f + yy, df_y = 126.0f + yy;
            float weapon_y = 156.0f + yy, armor_y = 172.0f + yy;
            float gold_y = 192.0f + yy, kills_y = 192.0f + yy;
            if (isJa) { weapon_y -= 2.0f; gold_y += 2.0f; kills_y += 2.0f; }

            char tmp[128];
            nativeDrawText(runner, r, stat_x, name_y, ovrctrl_getText(ctx, "stat_menu_name"));

            snprintf(tmp, sizeof(tmp), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gLv));
            char out[256]; ovrctrl_getTextArg(ctx, "stat_menu_lv", tmp, out, sizeof(out));
            nativeDrawText(runner, r, stat_x, lv_y, out);

            char hpA[16], hpB[16];
            snprintf(hpA, sizeof(hpA), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gHp));
            snprintf(hpB, sizeof(hpB), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gMaxhp));
            ovrctrl_getTextArg2(ctx, "stat_menu_hp", hpA, hpB, out, sizeof(out));
            nativeDrawText(runner, r, stat_x, hp_y, out);

            char atA[16], atB[16];
            snprintf(atA, sizeof(atA), "%d", (int32_t)(globalReal(ctx, ovrctrlCache.gAt) - 10.0));
            snprintf(atB, sizeof(atB), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gWstrength));
            ovrctrl_getTextArg2(ctx, "stat_menu_at", atA, atB, out, sizeof(out));
            nativeDrawText(runner, r, stat_x, at_y, out);

            char dfA[16], dfB[16];
            snprintf(dfA, sizeof(dfA), "%d", (int32_t)(globalReal(ctx, ovrctrlCache.gDf) - 10.0));
            snprintf(dfB, sizeof(dfB), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gAdef));
            ovrctrl_getTextArg2(ctx, "stat_menu_df", dfA, dfB, out, sizeof(out));
            nativeDrawText(runner, r, stat_x, df_y, out);

            
            char weapKey[32];
            snprintf(weapKey, sizeof(weapKey), "item_name_%d", (int32_t)globalReal(ctx, ovrctrlCache.gWeapon));
            char weapName[128];
            strncpy(weapName, ovrctrl_getText(ctx, weapKey), sizeof(weapName) - 1);
            weapName[sizeof(weapName) - 1] = '\0';
            ovrctrl_getTextArg(ctx, "stat_menu_weapon", weapName, out, sizeof(out));
            nativeDrawText(runner, r, stat_x, weapon_y, out);

            char armorKey[32];
            snprintf(armorKey, sizeof(armorKey), "item_name_%d", (int32_t)globalReal(ctx, ovrctrlCache.gArmor));
            char armName[128];
            if ((int32_t)globalReal(ctx, ovrctrlCache.gArmor) == 64) {
                strncpy(armName, ovrctrl_getText(ctx, "stat_armor_temmie"), sizeof(armName) - 1);
            } else {
                strncpy(armName, ovrctrl_getText(ctx, armorKey), sizeof(armName) - 1);
            }
            armName[sizeof(armName) - 1] = '\0';
            ovrctrl_getTextArg(ctx, "stat_menu_armor", armName, out, sizeof(out));
            nativeDrawText(runner, r, stat_x, armor_y, out);

            nativeDrawText(runner, r, stat_x, gold_y, ovrctrl_getText(ctx, "stat_menu_gold"));

            int32_t kills = (int32_t)globalReal(ctx, ovrctrlCache.gKills);
            if (kills > 20) {
                char killsS[16];
                snprintf(killsS, sizeof(killsS), "%d", kills);
                ovrctrl_getTextArg(ctx, "stat_menu_kills", killsS, out, sizeof(out));
                nativeDrawText(runner, r, kills_x, kills_y, out);
            }

            

            char xpS[16];
            snprintf(xpS, sizeof(xpS), "%d", (int32_t)globalReal(ctx, ovrctrlCache.gXp));
            ovrctrl_getTextArg(ctx, "stat_menu_exp", xpS, out, sizeof(out));
            nativeDrawText(runner, r, exp_x, at_y, out);

            
            static const int32_t xpThresh[] = {10,30,70,120,200,300,500,800,1200,1700,2500,3500,5000,7000,10000,15000,25000,50000,99999};
            int32_t lv = (int32_t)globalReal(ctx, ovrctrlCache.gLv);
            int32_t xp = (int32_t)globalReal(ctx, ovrctrlCache.gXp);
            int32_t nextlevel = 0;
            if (lv >= 1 && lv <= 19) nextlevel = xpThresh[lv - 1] - xp;
            Instance_setSelfVar(inst, ovrctrlCache.nextlevel, RValue_makeReal((GMLReal)nextlevel));
            char nlS[16];
            snprintf(nlS, sizeof(nlS), "%d", nextlevel);
            ovrctrl_getTextArg(ctx, "stat_menu_next", nlS, out, sizeof(out));
            nativeDrawText(runner, r, exp_x, df_y, out);
        }

        
        
        if (menuno == 4) {
            
            static int32_t scrRoomnameId = -2, scrDrawtextCenteredId = -2;
            static int32_t ossafeIniOpenId = -2, ossafeIniCloseId = -2;
            if (scrRoomnameId == -2) {
                scrRoomnameId = findScriptCodeId(ctx, "scr_roomname");
                scrDrawtextCenteredId = findScriptCodeId(ctx, "scr_drawtext_centered");
                ossafeIniOpenId = findScriptCodeId(ctx, "ossafe_ini_open");
                ossafeIniCloseId = findScriptCodeId(ctx, "ossafe_ini_close");
            }
            BuiltinFunc iniReadStr = VMBuiltins_find("ini_read_string");
            BuiltinFunc iniReadReal = VMBuiltins_find("ini_read_real");

            
            if (ossafeIniOpenId >= 0) {
                RValue args[1] = { RValue_makeString("undertale.ini") };
                RValue rr = VM_callCodeIndex(ctx, ossafeIniOpenId, args, 1);
                RValue_free(&rr);
            }

            
            char nameStr[128] = "";
            if (iniReadStr) {
                const char* defName = ovrctrl_getText(ctx, "save_menu_empty");
                RValue args[3] = { RValue_makeString("General"), RValue_makeString("Name"),
                                    RValue_makeString((char*)defName) };
                RValue rr = iniReadStr(ctx, args, 3);
                if (rr.type == RVALUE_STRING && rr.string) {
                    strncpy(nameStr, rr.string, sizeof(nameStr) - 1);
                    nameStr[sizeof(nameStr) - 1] = '\0';
                }
                RValue_free(&rr);
            }
            
            GMLReal love = 0, time_v = 1, killsV = 0, roomeV = 0;
            if (iniReadReal) {
                RValue a[3];
                a[0] = RValue_makeString("General"); a[1] = RValue_makeString("Love"); a[2] = RValue_makeReal(0);
                RValue rr = iniReadReal(ctx, a, 3); love = RValue_toReal(rr); RValue_free(&rr);
                a[1] = RValue_makeString("Time"); a[2] = RValue_makeReal(1);
                rr = iniReadReal(ctx, a, 3); time_v = RValue_toReal(rr); RValue_free(&rr);
                a[1] = RValue_makeString("Kills"); a[2] = RValue_makeReal(0);
                rr = iniReadReal(ctx, a, 3); killsV = RValue_toReal(rr); RValue_free(&rr);
                (void)killsV;
                a[1] = RValue_makeString("Room"); a[2] = RValue_makeReal(0);
                rr = iniReadReal(ctx, a, 3); roomeV = RValue_toReal(rr); RValue_free(&rr);
            }

            
            if (ossafeIniCloseId >= 0) {
                RValue rr = VM_callCodeIndex(ctx, ossafeIniCloseId, NULL, 0);
                RValue_free(&rr);
            }

            
            r->drawFont = isJa ? 14 : 2;

            
            r->drawColor = 0xFFFFFF;
            r->vtable->drawRectangle(r, 54.0f + xx, 49.0f + yy, 265.0f + xx, 135.0f + yy, 0xFFFFFF, r->drawAlpha, false);
            r->drawColor = 0;
            r->vtable->drawRectangle(r, 57.0f + xx, 52.0f + yy, 262.0f + xx, 132.0f + yy, 0, r->drawAlpha, false);
            r->drawColor = 0xFFFFFF;

            int32_t mc4 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 4);
            if (mc4 == 2) r->drawColor = 0x00FFFF; 

            
            int32_t minutes = (int32_t)(time_v / 1800.0);
            int32_t seconds = (int32_t)((((time_v / 1800.0) - (double)minutes) * 60.0) + 0.5);
            if (seconds == 60) seconds = 59;
            char secsStr[8];
            if (seconds < 10) snprintf(secsStr, sizeof(secsStr), "0%d", seconds);
            else snprintf(secsStr, sizeof(secsStr), "%d", seconds);

            
            char roomNameBuf[128] = "";
            if (scrRoomnameId >= 0) {
                RValue arg = RValue_makeReal(roomeV);
                RValue rr = VM_callCodeIndex(ctx, scrRoomnameId, &arg, 1);
                if (rr.type == RVALUE_STRING && rr.string) {
                    strncpy(roomNameBuf, rr.string, sizeof(roomNameBuf) - 1);
                    roomNameBuf[sizeof(roomNameBuf) - 1] = '\0';
                }
                RValue_free(&rr);
            }

            
            char loveStr[16], minutesStr[16];
            snprintf(loveStr, sizeof(loveStr), "%d", (int32_t)love);
            snprintf(minutesStr, sizeof(minutesStr), "%d", minutes);
            char lvtext[128], timetext[128];
            ovrctrl_getTextArg(ctx, "save_menu_lv", loveStr, lvtext, sizeof(lvtext));
            ovrctrl_getTextArg2(ctx, "save_menu_time", minutesStr, secsStr, timetext, sizeof(timetext));

            
            char nameFirst6[8];
            for (int32_t k = 0; k < 6; k++) {
                if (nameStr[k] == '\0') { nameFirst6[k] = '\0'; break; }
                nameFirst6[k] = nameStr[k];
            }
            nameFirst6[6] = '\0';
            float namesize = nativeStringWidth(runner, r, nameFirst6);
            float lvsize = nativeStringWidth(runner, r, lvtext);
            float timesize = nativeStringWidth(runner, r, timetext);
            float x_center = xx + 160.0f;
            float lvpos = roundf((x_center + (namesize / 2.0f)) - (timesize / 2.0f) - (lvsize / 2.0f));
            float namepos = 70.0f + xx;
            float timepos = 250.0f + xx;
            if (isJa) { namepos -= 6.0f; timepos += 6.0f; }

            nativeDrawText(runner, r, namepos, 60.0f + yy, nameStr);
            nativeDrawText(runner, r, lvpos, 60.0f + yy, lvtext);
            nativeDrawText(runner, r, timepos - timesize, 60.0f + yy, timetext);

            if (isJa && scrDrawtextCenteredId >= 0) {
                RValue args[3] = { RValue_makeReal(x_center), RValue_makeReal(80.0 + yy),
                                    RValue_makeString(roomNameBuf) };
                RValue rr = VM_callCodeIndex(ctx, scrDrawtextCenteredId, args, 3);
                RValue_free(&rr);
            } else {
                nativeDrawText(runner, r, namepos, 80.0f + yy, roomNameBuf);
            }

            float savepos = xx + 71.0f, returnpos = xx + 161.0f;
            if (isJa) { savepos = xx + 78.0f; returnpos = xx + 173.0f; }

            if (mc4 == 0) Renderer_drawSprite(r, 61, 0, savepos, yy + 113.0f);
            if (mc4 == 1) Renderer_drawSprite(r, 61, 0, returnpos, yy + 113.0f);

            if (mc4 < 2) {
                nativeDrawText(runner, r, savepos + 14.0f, yy + 110.0f, ovrctrl_getText(ctx, "save_menu_save"));
                nativeDrawText(runner, r, returnpos + 14.0f, yy + 110.0f, ovrctrl_getText(ctx, "save_menu_return"));
            } else {
                nativeDrawText(runner, r, xx + 85.0f, yy + 110.0f, ovrctrl_getText(ctx, "save_menu_saved"));
                if (ovrctrl_controlPressed(ctx, 0)) {
                    globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(-1));
                    globalSet(ctx, ovrctrlCache.gInteract, RValue_makeReal(0));
                    globalArraySet(ctx, ovrctrlCache.gMenucoord, 4, RValue_makeReal(0));
                    ovrctrl_controlClear(ctx, 0);
                }
            }

            
            BuiltinFunc kbPressed4 = VMBuiltins_find("keyboard_check_pressed");
            if (kbPressed4) {
                RValue k37 = RValue_makeReal(37);
                RValue rr37 = kbPressed4(ctx, &k37, 1);
                bool left = RValue_toReal(rr37) != 0; RValue_free(&rr37);
                RValue k39 = RValue_makeReal(39);
                RValue rr39 = kbPressed4(ctx, &k39, 1);
                bool right = RValue_toReal(rr39) != 0; RValue_free(&rr39);
                if ((left || right) && mc4 < 2) {
                    globalArraySet(ctx, ovrctrlCache.gMenucoord, 4,
                                   RValue_makeReal((mc4 == 1) ? 0 : 1));
                    BuiltinFunc kbClear = VMBuiltins_find("keyboard_clear");
                    if (kbClear) {
                        RValue k = RValue_makeReal(37); RValue rr = kbClear(ctx, &k, 1); RValue_free(&rr);
                        k = RValue_makeReal(39); rr = kbClear(ctx, &k, 1); RValue_free(&rr);
                    }
                }
            }

            
            if (ovrctrl_controlPressed(ctx, 0) && mc4 == 1) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(-1));
                globalSet(ctx, ovrctrlCache.gInteract, RValue_makeReal(0));
                globalArraySet(ctx, ovrctrlCache.gMenucoord, 4, RValue_makeReal(0));
                ovrctrl_controlClear(ctx, 0);
            }
            
            if (ovrctrl_controlPressed(ctx, 1)) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(-1));
                globalSet(ctx, ovrctrlCache.gInteract, RValue_makeReal(0));
                globalArraySet(ctx, ovrctrlCache.gMenucoord, 4, RValue_makeReal(0));
                ovrctrl_controlClear(ctx, 1);
            }
        }

        
        if (menuno == 0) {
            float heart_y = isJa ? 87.0f : 88.0f;
            int32_t mc0 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 0);
            Renderer_drawSprite(r, 61, 0, 28.0f + xx, heart_y + yy + (18.0f * mc0));
        }
        if (menuno == 1) {
            float heart_y = isJa ? 33.0f : 34.0f;
            int32_t mc = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 1);
            Renderer_drawSprite(r, 61, 0, 104.0f + xx, heart_y + yy + (16.0f * mc));
        }
        if (menuno == 3) {
            float heart_y = isJa ? 33.0f : 34.0f;
            int32_t mc = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 3);
            Renderer_drawSprite(r, 61, 0, 104.0f + xx, heart_y + yy + (16.0f * mc));
        }
        if (menuno == 6) {
            float heart_y = isJa ? 33.0f : 34.0f;
            int32_t mc = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 6);
            Renderer_drawSprite(r, 61, 0, 104.0f + xx, heart_y + yy + (16.0f * mc));
        }
        if (menuno == 7) {
            float heart_y = isJa ? 33.0f : 34.0f;
            int32_t mc = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 7);
            Renderer_drawSprite(r, 61, 0, 104.0f + xx, heart_y + yy + (16.0f * mc));
        }
        if (menuno == 5) {
            float heart_y = isJa ? 173.0f : 174.0f;
            int32_t mc = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 5);
            float hx = 104.0f + xx + (45.0f * (float)mc);
            if (mc == 1) hx += 3.0f;
            else if (mc == 2) hx += 15.0f;
            Renderer_drawSprite(r, 61, 0, hx, heart_y + yy);
        }

        
        BuiltinFunc kbPressed = VMBuiltins_find("keyboard_check_pressed");

        if (ovrctrl_controlPressed(ctx, 0)) {
            if (menuno == 5) {
                int32_t mc5 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 5);
                int32_t mc1 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 1);
                GMLReal item = getGlobalArray(ctx, ovrctrlCache.gItem, mc1);
                if (mc5 == 0 && ovrctrlCache.scrItemuseb >= 0) {
                    globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(9));
                    RValue args[2] = { RValue_makeReal((GMLReal)mc1), RValue_makeReal(item) };
                    RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrItemuseb, args, 2);
                    RValue_free(&rr);
                }
                else if (mc5 == 1 && ovrctrlCache.scrItemdesc >= 0 && ovrctrlCache.scrWritetext >= 0) {
                    globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(9));
                    RValue a1 = RValue_makeReal(item);
                    RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrItemdesc, &a1, 1);
                    RValue_free(&rr);
                    RValue wt[4] = { RValue_makeReal(0), RValue_makeString("x"), RValue_makeReal(0), RValue_makeReal(0) };
                    rr = VM_callCodeIndex(ctx, ovrctrlCache.scrWritetext, wt, 4);
                    RValue_free(&rr);
                }
                
            }
            if (menuno == 3 && ovrctrlCache.scrItemuseb >= 0) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(9));
                int32_t mc3 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 3);
                GMLReal phoneItem = getGlobalArray(ctx, ovrctrlCache.gPhone, mc3);
                RValue args[2] = { RValue_makeReal((GMLReal)mc3), RValue_makeReal(phoneItem) };
                RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrItemuseb, args, 2);
                RValue_free(&rr);
            }
            if (menuno == 6 && ovrctrlCache.scrStorageget >= 0) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(9));
                int32_t mc6 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 6);
                GMLReal it = getGlobalArray(ctx, ovrctrlCache.gItem, mc6);
                RValue args[2] = { RValue_makeReal(it), RValue_makeReal(300) };
                RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrStorageget, args, 2);
                RValue_free(&rr);
                
            }
            if (menuno == 7 && ovrctrlCache.scrItemget >= 0) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(9));
                int32_t mc7 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 7);
                GMLReal flagV = getGlobalArray(ctx, ovrctrlCache.gFlag, mc7 + 300);
                RValue arg = RValue_makeReal(flagV);
                RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrItemget, &arg, 1);
                RValue_free(&rr);
            }
            if (menuno == 1) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(5));
                globalArraySet(ctx, ovrctrlCache.gMenucoord, 5, RValue_makeReal(0));
            }
            if (menuno == 0) {
                int32_t mc0 = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 0);
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal((GMLReal)(menuno + mc0 + 1)));
            }
            
            int32_t newMenuno = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
            if (newMenuno == 3 && ovrctrlCache.scrPhonename >= 0) {
                RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrPhonename, NULL, 0);
                RValue_free(&rr);
                globalArraySet(ctx, ovrctrlCache.gMenucoord, 3, RValue_makeReal(0));
            }
            if (newMenuno == 1) {
                if ((int32_t)getGlobalArray(ctx, ovrctrlCache.gItem, 0) != 0) {
                    globalArraySet(ctx, ovrctrlCache.gMenucoord, 1, RValue_makeReal(0));
                    if (ovrctrlCache.scrItemname >= 0) {
                        RValue rr = VM_callCodeIndex(ctx, ovrctrlCache.scrItemname, NULL, 0);
                        RValue_free(&rr);
                    }
                } else {
                    globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(0));
                }
            }
        }

        
        if (kbPressed) {
            RValue k38 = RValue_makeReal(38);
            RValue rr = kbPressed(ctx, &k38, 1);
            bool up = RValue_toReal(rr) != 0; RValue_free(&rr);
            RValue k40 = RValue_makeReal(40);
            rr = kbPressed(ctx, &k40, 1);
            bool down = RValue_toReal(rr) != 0; RValue_free(&rr);
            RValue k37 = RValue_makeReal(37);
            rr = kbPressed(ctx, &k37, 1);
            bool left = RValue_toReal(rr) != 0; RValue_free(&rr);
            RValue k39 = RValue_makeReal(39);
            rr = kbPressed(ctx, &k39, 1);
            bool right = RValue_toReal(rr) != 0; RValue_free(&rr);

            int32_t curMenuno = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
            if (up) {
                if (curMenuno == 0) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 0);
                    if (v != 0) globalArraySet(ctx, ovrctrlCache.gMenucoord, 0, RValue_makeReal((GMLReal)(v - 1)));
                } else if (curMenuno == 1) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 1);
                    if (v != 0) globalArraySet(ctx, ovrctrlCache.gMenucoord, 1, RValue_makeReal((GMLReal)(v - 1)));
                } else if (curMenuno == 3) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 3);
                    if (v != 0) globalArraySet(ctx, ovrctrlCache.gMenucoord, 3, RValue_makeReal((GMLReal)(v - 1)));
                } else if (curMenuno == 6) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 6);
                    if (v != 0) globalArraySet(ctx, ovrctrlCache.gMenucoord, 6, RValue_makeReal((GMLReal)(v - 1)));
                } else if (curMenuno == 7) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 7);
                    if (v != 0) globalArraySet(ctx, ovrctrlCache.gMenucoord, 7, RValue_makeReal((GMLReal)(v - 1)));
                }
            }
            if (down) {
                if (curMenuno == 0) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 0);
                    if (v != 2 && (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenuchoice, v + 1) != 0)
                        globalArraySet(ctx, ovrctrlCache.gMenucoord, 0, RValue_makeReal((GMLReal)(v + 1)));
                } else if (curMenuno == 1) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 1);
                    if (v != 7 && (int32_t)getGlobalArray(ctx, ovrctrlCache.gItem, v + 1) != 0)
                        globalArraySet(ctx, ovrctrlCache.gMenucoord, 1, RValue_makeReal((GMLReal)(v + 1)));
                } else if (curMenuno == 3) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 3);
                    if (v != 7 && (int32_t)getGlobalArray(ctx, ovrctrlCache.gPhone, v + 1) != 0)
                        globalArraySet(ctx, ovrctrlCache.gMenucoord, 3, RValue_makeReal((GMLReal)(v + 1)));
                } else if (curMenuno == 6) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 6);
                    if (v != 7 && (int32_t)getGlobalArray(ctx, ovrctrlCache.gItem, v + 1) != 0)
                        globalArraySet(ctx, ovrctrlCache.gMenucoord, 6, RValue_makeReal((GMLReal)(v + 1)));
                } else if (curMenuno == 7) {
                    int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 7);
                    if (v != 9 && (int32_t)getGlobalArray(ctx, ovrctrlCache.gFlag, v + 301) != 0)
                        globalArraySet(ctx, ovrctrlCache.gMenucoord, 7, RValue_makeReal((GMLReal)(v + 1)));
                }
            }
            if (right && curMenuno == 5) {
                int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 5);
                if (v != 2) globalArraySet(ctx, ovrctrlCache.gMenucoord, 5, RValue_makeReal((GMLReal)(v + 1)));
            }
            if (left && curMenuno == 5) {
                int32_t v = (int32_t)getGlobalArray(ctx, ovrctrlCache.gMenucoord, 5);
                if (v != 0) globalArraySet(ctx, ovrctrlCache.gMenucoord, 5, RValue_makeReal((GMLReal)(v - 1)));
            }
        }

        
        if (ovrctrl_controlPressed(ctx, 1) && buffer >= 0) {
            int32_t curMenuno = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
            if (curMenuno == 0) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(-1));
                globalSet(ctx, ovrctrlCache.gInteract, RValue_makeReal(0));
            } else if (curMenuno <= 3) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(0));
            }
            if (curMenuno == 5) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(1));
            }
        }
        
        if (ovrctrl_controlPressed(ctx, 2)) {
            int32_t curMenuno = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
            if (curMenuno == 0) {
                globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(-1));
                globalSet(ctx, ovrctrlCache.gInteract, RValue_makeReal(0));
            }
        }

        
        int32_t finalMenuno = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
        BuiltinFunc aps = VMBuiltins_find("audio_play_sound");
        if (currentmenu < finalMenuno && finalMenuno != 9) {
            if (aps) {
                RValue a[3] = { RValue_makeReal(112), RValue_makeReal(80), RValue_makeReal(0) };
                RValue rr = aps(ctx, a, 3); RValue_free(&rr);
            }
        } else if (finalMenuno >= 0 && finalMenuno < 6) {
            GMLReal newSpot = getGlobalArray(ctx, ovrctrlCache.gMenucoord, finalMenuno);
            if (currentspot != newSpot) {
                if (aps) {
                    RValue a[3] = { RValue_makeReal(115), RValue_makeReal(80), RValue_makeReal(0) };
                    RValue rr = aps(ctx, a, 3); RValue_free(&rr);
                }
            }
        }
    } 

    
    int32_t finalMenuno2 = (int32_t)globalReal(ctx, ovrctrlCache.gMenuno);
    if (finalMenuno2 == 9 && findInstanceByObject(runner, 780) == NULL) {
        globalSet(ctx, ovrctrlCache.gMenuno, RValue_makeReal(-1));
        globalSet(ctx, ovrctrlCache.gInteract, RValue_makeReal(0));
    }
}







static struct {
    int32_t siner, sinerfactor, moved;
    bool ready;
} waterdivotCache = { .ready = false };

static void initWaterdivotCache(DataWin* dw) {
    waterdivotCache.siner = findSelfVarId(dw, "siner");
    waterdivotCache.sinerfactor = findSelfVarId(dw, "sinerfactor");
    waterdivotCache.moved = findSelfVarId(dw, "moved");
    waterdivotCache.ready = (waterdivotCache.siner >= 0 && waterdivotCache.sinerfactor >= 0 &&
                             waterdivotCache.moved >= 0);
}

static void native_waterdivot_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!waterdivotCache.ready || !runner->currentRoom) return;

    GMLReal siner = selfReal(inst, waterdivotCache.siner);
    GMLReal sinerfactor = selfReal(inst, waterdivotCache.sinerfactor);

    inst->x += (float)(GMLReal_sin(siner / 6.0) * sinerfactor);
    siner += 1.0;
    Instance_setSelfVar(inst, waterdivotCache.siner, RValue_makeReal(siner));

    float roomWidth = (float)runner->currentRoom->width;
    if (inst->x > roomWidth) {
        inst->x = -10.0f;
        if (runner->currentRoomIndex == 68) inst->x = 2300.0f;
    }

    if (runner->currentRoomIndex == 82) {
        int32_t moved = selfInt(inst, waterdivotCache.moved);
        
        if (moved == 0) {
            bool trigger = (inst->y <= 50.0f && inst->x >= 460.0f) ||
                           (inst->y <= 70.0f && inst->x >= 480.0f) ||
                           (inst->y <= 90.0f && inst->x >= 500.0f);
            if (trigger) {
                inst->vspeed = -inst->hspeed;
                inst->hspeed = 0.0f;
                Instance_computeSpeedFromComponents(inst);
                Instance_setSelfVar(inst, waterdivotCache.moved, RValue_makeReal(1.0));
            }
        }
        if (inst->y <= -15.0f) {
            Instance_setSelfVar(inst, waterdivotCache.moved, RValue_makeReal(0.0));
            inst->x = -10.0f;
            inst->hspeed = 1.5f;
            inst->y = inst->ystart;
            inst->vspeed = 0.0f;
            Instance_computeSpeedFromComponents(inst);
        }
    }
}







static struct {
    int32_t drawn, active, osc, oscmax, oscmin;
    int32_t gTurntimer;
    bool ready;
} sizeboneCache = { .ready = false };

static void initSizeboneCache(VMContext* ctx, DataWin* dw) {
    sizeboneCache.drawn = findSelfVarId(dw, "drawn");
    sizeboneCache.active = findSelfVarId(dw, "active");
    sizeboneCache.osc = findSelfVarId(dw, "osc");
    sizeboneCache.oscmax = findSelfVarId(dw, "oscmax");
    sizeboneCache.oscmin = findSelfVarId(dw, "oscmin");
    sizeboneCache.gTurntimer = findGlobalVarId(ctx, "turntimer");
    sizeboneCache.ready = (sizeboneCache.drawn >= 0 && sizeboneCache.active >= 0 &&
                           sizeboneCache.osc >= 0 && sizeboneCache.oscmax >= 0 &&
                           sizeboneCache.oscmin >= 0 && sizeboneCache.gTurntimer >= 0);
}

static void native_sizebone_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    if (!sizeboneCache.ready) return;
    
    
    
    
    
    if (selfInt(inst, sizeboneCache.drawn) != 1) return;
    if (selfInt(inst, sizeboneCache.active) != 1) return;

    GMLReal osc = selfReal(inst, sizeboneCache.osc);
    GMLReal oscmax = selfReal(inst, sizeboneCache.oscmax);
    GMLReal oscmin = selfReal(inst, sizeboneCache.oscmin);

    float topEdge = inst->ystart - (float)oscmax;
    float botEdge = inst->ystart - (float)oscmin;
    if (inst->y <= topEdge || inst->y >= botEdge) {
        osc = -osc;
        Instance_setSelfVar(inst, sizeboneCache.osc, RValue_makeReal(osc));
    }
    inst->y += (float)osc;
}

static void native_sizebone_Step2(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!sizeboneCache.ready) return;
    
    if (globalReal(ctx, sizeboneCache.gTurntimer) >= 0.0) return;
    if (selfInt(inst, sizeboneCache.active) != 1) return;
    Runner_destroyInstance(runner, inst);
}







static struct {
    int32_t blue, drawn_s;          
    int32_t gIdealborder, gInvc;    
    int32_t objHeart;               
    int32_t spriteBoneTop, spriteBoneBot;
    bool ready;
} sizeboneDrawCache = { .ready = false };

static void initSizeboneDrawCache(VMContext* ctx, DataWin* dw) {
    sizeboneDrawCache.blue = findSelfVarId(dw, "blue");
    sizeboneDrawCache.drawn_s = findSelfVarId(dw, "drawn");
    sizeboneDrawCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    sizeboneDrawCache.gInvc = findGlobalVarId(ctx, "invc");
    sizeboneDrawCache.objHeart = 744;
    sizeboneDrawCache.spriteBoneTop = 124;
    sizeboneDrawCache.spriteBoneBot = 123;
    sizeboneDrawCache.ready = (sizeboneDrawCache.blue >= 0 && sizeboneDrawCache.drawn_s >= 0 &&
                               sizeboneDrawCache.gIdealborder >= 0);
}

static void native_sizebone_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!sizeboneDrawCache.ready) return;
    Renderer* r = runner->renderer;
    DataWin* dw = ctx->dataWin;
    if (!r || inst->spriteIndex < 0 || (uint32_t)inst->spriteIndex >= dw->sprt.count) return;

    
    GMLReal b0 = getGlobalArray(ctx, sizeboneDrawCache.gIdealborder, 0);
    GMLReal b1 = getGlobalArray(ctx, sizeboneDrawCache.gIdealborder, 1);
    GMLReal b2 = getGlobalArray(ctx, sizeboneDrawCache.gIdealborder, 2);
    GMLReal b3 = getGlobalArray(ctx, sizeboneDrawCache.gIdealborder, 3);

    int32_t blue = selfInt(inst, sizeboneDrawCache.blue);

    
    Sprite* spr = &dw->sprt.sprites[inst->spriteIndex];
    float sw = (float)spr->width * inst->imageXscale;
    float sh = (float)spr->height * inst->imageYscale;

    
    float l = 0, t = 0, w = sw, h = sh;
    float ll = ((float)b0 - inst->x) + 1.0f;
    float tt = ((float)b2 - inst->y) + 1.0f;
    float ww = (inst->x + w) - (float)b1 - 1.0f;
    float hh = (inst->y + h) - (float)b3 - 1.0f;
    if (ll > 0) l += ll;
    if (tt > 0) t += tt;
    if (ww > 0) w -= ww;
    if (hh > 0) h -= hh;
    w = GMLReal_round(w); h = GMLReal_round(h);
    l = GMLReal_round(l); t = GMLReal_round(t);

    
    if (w > 0 && h > 0 && l < w && t < h) {
        int32_t imgIdx = (int32_t)inst->imageIndex;
        if (blue == 1) imgIdx = 1;
        int32_t tpagTop = Renderer_resolveTPAGIndex(dw, sizeboneDrawCache.spriteBoneTop, imgIdx);
        int32_t tpagBot = Renderer_resolveTPAGIndex(dw, sizeboneDrawCache.spriteBoneBot, imgIdx);
        if (tpagTop >= 0) {
            r->vtable->drawSpritePart(r, tpagTop,
                (int32_t)l, (int32_t)t, (int32_t)(w - l), (int32_t)(h - t),
                inst->x + l, inst->y + t, 1.0f, 1.0f, 0xFFFFFF, inst->imageAlpha);
        }
        if (tpagBot >= 0) {
            r->vtable->drawSpritePart(r, tpagBot,
                (int32_t)l, (int32_t)t, (int32_t)(w - l), (int32_t)(h - t),
                inst->x + l, (float)b3 - 10.0f, 1.0f, 1.0f, 0xFFFFFF, inst->imageAlpha);
        }
    }

    
    
    if (inst->x > ((float)b0 - 5.0f) && inst->x < ((float)b1 - 4.0f)) {
        Instance_setSelfVar(inst, sizeboneDrawCache.drawn_s, RValue_makeReal(1.0));
        
        
        uint32_t color = (blue == 1) ? 0xFFA914u : 0xFFFFFFu;
        r->drawColor = color;
        r->vtable->drawRectangle(r, inst->x + 3.0f, inst->y + 4.0f,
                                    inst->x + 9.0f, (float)b3 - 6.0f,
                                    color, r->drawAlpha, false);
    }

    
    
    if (sizeboneDrawCache.gInvc >= 0 && globalReal(ctx, sizeboneDrawCache.gInvc) < 1.0) {
        updateHeartCache(runner);
        if (heartCache.inst != NULL && heartCache.bbox.valid) {
            if (fabsf((float)heartCache.inst->x - inst->x) < 15.0f) {
                float rx1 = inst->x + 3.0f, ry1 = inst->y + 2.0f;
                float rx2 = inst->x + 9.0f, ry2 = (float)b3 - 2.0f;
                InstanceBBox h = heartCache.bbox;
                
                if (!(rx1 >= h.right || h.left >= rx2 || ry1 >= h.bottom || h.top >= ry2)) {
                    Runner_executeEvent(runner, inst, 7, 11); 
                }
            }
        }
    }

    
    if (inst->x < ((float)b0 - 10.0f) && inst->hspeed < 0) {
        Runner_destroyInstance(runner, inst);
        return;
    }
    if (inst->x > ((float)b1 + 10.0f) && inst->hspeed > 0) {
        Runner_destroyInstance(runner, inst);
        return;
    }
}







static void native_whtpxlgrav_Create0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    inst->gravityDirection = 90.0f;
    inst->gravity = 0.2f + ((float)rand() / (float)RAND_MAX) * 0.5f;
    inst->hspeed = ((float)rand() / (float)RAND_MAX) * 4.0f - 2.0f;
    Instance_computeSpeedFromComponents(inst);
}







static struct {
    int32_t ht, wd, myvapor, myread, finishedreading, line, spec;
    int32_t mydata;                         
    int32_t wwSelf, mycharSelf, funSelf;    
    int32_t objWhtpxlgrav;                  
    bool ready;
} vapNewCache = { .ready = false };

static void initVapNewCache(VMContext* ctx, DataWin* dw) {
    (void)ctx;
    vapNewCache.ht = findSelfVarId(dw, "ht");
    vapNewCache.wd = findSelfVarId(dw, "wd");
    vapNewCache.myvapor = findSelfVarId(dw, "myvapor");
    vapNewCache.myread = findSelfVarId(dw, "myread");
    vapNewCache.finishedreading = findSelfVarId(dw, "finishedreading");
    vapNewCache.line = findSelfVarId(dw, "line");
    vapNewCache.spec = findSelfVarId(dw, "spec");
    vapNewCache.mydata = findSelfVarId(dw, "mydata");
    vapNewCache.wwSelf = findSelfVarId(dw, "ww");
    vapNewCache.mycharSelf = findSelfVarId(dw, "mychar");
    vapNewCache.funSelf = findSelfVarId(dw, "fun");
    vapNewCache.objWhtpxlgrav = 192;
    vapNewCache.ready = (vapNewCache.ht >= 0 && vapNewCache.wd >= 0 && vapNewCache.line >= 0 &&
                         vapNewCache.mydata >= 0 && vapNewCache.myread >= 0 &&
                         vapNewCache.finishedreading >= 0);
}

static void native_vapNew_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!vapNewCache.ready) return;
    Renderer* r = runner->renderer;
    if (!r) return;

    
    int32_t line = selfInt(inst, vapNewCache.line);
    int32_t wd = selfInt(inst, vapNewCache.wd);
    int32_t ht = selfInt(inst, vapNewCache.ht);
    int32_t myvapor = selfInt(inst, vapNewCache.myvapor);
    int32_t spec = selfInt(inst, vapNewCache.spec);
    int32_t finishedreading = selfInt(inst, vapNewCache.finishedreading);

    DataWin* dw = ctx->dataWin;
    if (inst->spriteIndex >= 0 && (uint32_t)inst->spriteIndex < dw->sprt.count) {
        Sprite* spr = &dw->sprt.sprites[inst->spriteIndex];
        float sprHeight = (float)spr->height;
        float ht_a = sprHeight - (float)(line * 2);
        float ht_b = inst->y + (float)(line * 2);
        float ht_c = (float)(line * 2);
        int32_t tpagIdx = Renderer_resolveTPAGIndex(dw, inst->spriteIndex, (int32_t)inst->imageIndex);
        if (tpagIdx >= 0) {
            r->vtable->drawSpritePart(r, tpagIdx,
                0, (int32_t)ht_c, wd, (int32_t)ht_a,
                inst->x, ht_b, inst->imageXscale, inst->imageYscale, 0xFFFFFF, 1.0f);
        }
    }

    if (finishedreading != 0) return;

    
    const char* mydata = selfString(inst, vapNewCache.mydata);
    int32_t mydataLen = (int32_t)strlen(mydata);
    int32_t myread = selfInt(inst, vapNewCache.myread);

    char mychar = '0';
    
    for (int32_t rpt = 0; rpt < 4; rpt++) {
        int32_t ww = 0;
        mychar = '0';

        while (mychar != '}' && mychar != '~') {
            if (myread >= mydataLen) {
                mychar = '~';  
                break;
            }
            mychar = mydata[myread];
            int32_t c = (unsigned char)mychar;

            if (c >= 84 && c <= 121) {
                
                ww += (c - 85) * 2;
            } else if (c >= 39 && c <= 82) {
                int32_t n = c - 40;
                if (wd > 120 && spec == 0) {
                    
                    Instance* blk = Runner_createInstance(runner,
                        inst->x + (float)ww, inst->y + (float)(line * 2),
                        vapNewCache.objWhtpxlgrav);
                    if (blk) {
                        blk->imageXscale = (float)n;
                        
                        
                        Runner_executeEvent(runner, blk, 7, 10); 
                    }
                    ww += n * 2;
                } else {
                    
                    for (int32_t i = 0; i < n; i++) {
                        Runner_createInstance(runner,
                            inst->x + (float)ww, inst->y + (float)(line * 2) + 2.0f,
                            vapNewCache.objWhtpxlgrav);
                        ww += 2;
                    }
                }
            }

            myread += 1;
        }

        line += 1;

        if (mychar == '~') {
            Instance_setSelfVar(inst, vapNewCache.finishedreading, RValue_makeReal(1.0));
            Instance_setSelfVar(inst, vapNewCache.line, RValue_makeReal((GMLReal)line));
            Instance_setSelfVar(inst, vapNewCache.myread, RValue_makeReal((GMLReal)myread));
            Runner_destroyInstance(runner, inst);
            return;
        } else {
            inst->alarm[0] = 1 + myvapor;
        }
    }

    
    Instance_setSelfVar(inst, vapNewCache.line, RValue_makeReal((GMLReal)line));
    Instance_setSelfVar(inst, vapNewCache.myread, RValue_makeReal((GMLReal)myread));
    if (vapNewCache.mycharSelf >= 0) {
        char tmp[2] = { mychar, 0 };
        Instance_setSelfVar(inst, vapNewCache.mycharSelf, RValue_makeString(tmp));
    }
}







static void native_vaporized_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!vapNewCache.ready) return;
    if (selfInt(inst, vapNewCache.finishedreading) == 1) {
        Runner_destroyInstance(runner, inst);
        return;
    }
    if (selfInt(inst, vapNewCache.line) > 10) {
        
        if (findInstanceByObject(runner, 194) == NULL) {
            Runner_destroyInstance(runner, inst);
        }
    }
}





static struct {
    int32_t size;
    bool ready;
} bouncersteamCache = { .ready = false };

static void initBouncersteamCache(DataWin* dw) {
    bouncersteamCache.size = findSelfVarId(dw, "size");
    bouncersteamCache.ready = (bouncersteamCache.size >= 0);
}

static void native_bouncersteam_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!bouncersteamCache.ready) return;

    GMLReal size = selfReal(inst, bouncersteamCache.size) + 0.08;
    Instance_setSelfVar(inst, bouncersteamCache.size, RValue_makeReal(size));
    inst->imageXscale = (float)size;
    inst->imageYscale = (float)size;
    inst->imageAlpha -= 0.07f;
    if (inst->imageAlpha < 0.1f) {
        Runner_destroyInstance(runner, inst);
        return;
    }
    inst->imageAngle += 6.0f;
}
















static struct {
    int32_t btime, con, goldshift, gold;
    int32_t codeId;
    bool ready;
} bouncerightCache = { .ready = false };

static void initBouncerightCache(VMContext* ctx, DataWin* dw) {
    (void)ctx;
    bouncerightCache.btime     = findSelfVarId(dw, "btime");
    bouncerightCache.con       = findSelfVarId(dw, "con");
    bouncerightCache.goldshift = findSelfVarId(dw, "goldshift");
    bouncerightCache.gold      = findSelfVarId(dw, "gold");
    bouncerightCache.codeId = -1;
    for (uint32_t ci = 0; ci < dw->code.count; ci++) {
        if (strcmp(dw->code.entries[ci].name, "gml_Object_obj_bounceright_Step_0") == 0) {
            bouncerightCache.codeId = (int32_t)ci; break;
        }
    }
    bouncerightCache.ready = (bouncerightCache.btime >= 0 && bouncerightCache.con >= 0 &&
                              bouncerightCache.goldshift >= 0 && bouncerightCache.gold >= 0 &&
                              bouncerightCache.codeId >= 0);
}

static void native_bounceright_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)runner;
    if (!bouncerightCache.ready) return;

    int32_t con       = selfInt(inst, bouncerightCache.con);
    int32_t goldshift = selfInt(inst, bouncerightCache.goldshift);
    int32_t gold      = selfInt(inst, bouncerightCache.gold);

    
    if (con == 0 && !(goldshift == 1 && gold == 1)) {
        GMLReal btime = selfReal(inst, bouncerightCache.btime) - 1.0;
        Instance_setSelfVar(inst, bouncerightCache.btime, RValue_makeReal(btime));
        return;
    }

    
    
    
    Instance* saved = (Instance*)ctx->currentInstance;
    ctx->currentInstance = inst;
    RValue r = VM_executeCode(ctx, bouncerightCache.codeId);
    RValue_free(&r);
    ctx->currentInstance = saved;
}









static struct {
    int32_t ck, siner, cogno, offx, offy;
    bool ready;
} cogsmallCache = { .ready = false };

static void initCogsmallCache(DataWin* dw) {
    cogsmallCache.ck     = findSelfVarId(dw, "ck");
    cogsmallCache.siner  = findSelfVarId(dw, "siner");
    cogsmallCache.cogno  = findSelfVarId(dw, "cogno");
    cogsmallCache.offx   = findSelfVarId(dw, "offx");
    cogsmallCache.offy   = findSelfVarId(dw, "offy");
    cogsmallCache.ready  = (cogsmallCache.ck >= 0 && cogsmallCache.siner >= 0 &&
                            cogsmallCache.cogno >= 0 && cogsmallCache.offx >= 0 &&
                            cogsmallCache.offy >= 0);
}

static void native_cogsmall_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!cogsmallCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t ck = selfInt(inst, cogsmallCache.ck);
    GMLReal siner = selfReal(inst, cogsmallCache.siner);
    if (ck == 0) siner += 6.0;
    else if (ck == 1) siner -= 6.0;
    Instance_setSelfVar(inst, cogsmallCache.siner, RValue_makeReal(siner));

    
    
    
    int32_t cogno = selfInt(inst, cogsmallCache.cogno);
    if (cogno > 0 && r->vtable->drawCircle != NULL) {
        int32_t prec = (inst->imageXscale >= 2.0f) ? 8 : 4;
        int32_t savedPrec = r->circlePrecision;
        r->circlePrecision = prec;

        float offCenter = (inst->imageXscale < 2.0f) ? 0.3f : 0.4f;
        float ixs = inst->imageXscale;
        float iys = inst->imageYscale;
        float cx0 = inst->x - offCenter * ixs;
        float cy0 = inst->y - offCenter * iys;
        float circleRadius = 2.0f * ixs;
        for (int32_t i = 0; i < cogno; i++) {
            
            float ang_deg = (float)i / (float)cogno * 360.0f + (float)siner;
            float ang_rad = ang_deg * (float)(M_PI / 180.0);
            
            float ldx = 8.0f * GMLReal_cos(ang_rad);
            float ldy = -8.0f * GMLReal_sin(ang_rad);
            r->vtable->drawCircle(r, cx0 + ldx * ixs, cy0 + ldy * iys,
                                  circleRadius, 0x000080u, 1.0f, false, prec);
        }
        r->circlePrecision = savedPrec;
    }

    float offx = (float)selfReal(inst, cogsmallCache.offx);
    float offy = (float)selfReal(inst, cogsmallCache.offy);
    if (inst->imageXscale < 2.0f) {
        Renderer_drawSpriteExt(r, inst->spriteIndex, 0,
                               inst->x + offx, inst->y + offy,
                               inst->imageXscale, inst->imageYscale,
                               0.0f, 0xFFFFFFu, 1.0f);
    } else {
        Renderer_drawSpriteExt(r, 976, 0,
                               inst->x + offx, inst->y + offy,
                               inst->imageXscale * 0.5f, inst->imageYscale * 0.5f,
                               0.0f, 0xFFFFFFu, 1.0f);
    }
}






static struct {
    int32_t siner;
    bool ready;
} hotlandBottomCache = { .ready = false };

static void initHotlandBottomCache(DataWin* dw) {
    hotlandBottomCache.siner = findSelfVarId(dw, "siner");
    hotlandBottomCache.ready = (hotlandBottomCache.siner >= 0);
}

static void native_hotlandBottom_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!hotlandBottomCache.ready || runner->renderer == NULL) return;

    GMLReal siner = selfReal(inst, hotlandBottomCache.siner) + 1.0;
    Instance_setSelfVar(inst, hotlandBottomCache.siner, RValue_makeReal(siner));
    int32_t subimg = (int32_t)(siner * 0.5);
    Renderer* r = runner->renderer;

    
    
    
    if (inst->imageXscale >= 0.0f) {
        float endI = inst->imageXscale - 1.0f;  
        for (int32_t i = 0; (float)i < inst->imageXscale; i++) {
            int32_t sprite;
            if (i == 0) sprite = 980;
            else if ((float)i == endI) sprite = 983;
            else sprite = 979;
            Renderer_drawSprite(r, sprite, subimg, inst->x + (float)(i * 20), inst->y);
        }
        
        if (runner->currentRoomIndex == 171) {
            Renderer_drawSpriteExt(r, 978, 0, inst->x, inst->y + 19.0f,
                                   20.0f * inst->imageXscale, 1.0f,
                                   0.0f, 0xFFFFFFu, 1.0f);
        }
    } else {
        Renderer_drawSprite(r, 983, subimg, inst->x - 20.0f, inst->y);
        float limit = -inst->imageXscale;
        for (int32_t i = 1; (float)i < limit; i++) {
            Renderer_drawSprite(r, 979, subimg, inst->x - (float)(i * 20) - 20.0f, inst->y);
        }
    }
}






static struct {
    int32_t siner;
    bool ready;
} hotlandRedCache = { .ready = false };

static void initHotlandRedCache(DataWin* dw) {
    hotlandRedCache.siner = findSelfVarId(dw, "siner");
    hotlandRedCache.ready = (hotlandRedCache.siner >= 0);
}

static void native_hotlandRed_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!hotlandRedCache.ready || runner->renderer == NULL) return;

    GMLReal siner = selfReal(inst, hotlandRedCache.siner) + 1.0;
    Instance_setSelfVar(inst, hotlandRedCache.siner, RValue_makeReal(siner));

    
    float alpha = (float)GMLReal_sin(siner / 16.0);
    if (alpha < 0.0f) alpha = -alpha;

    Renderer* r = runner->renderer;
    int32_t subimg = (int32_t)inst->imageIndex;
    
    
    for (int32_t i = 0; (float)i < inst->imageYscale; i++) {
        int32_t sprite = (i == 0) ? 982 : inst->spriteIndex;
        Renderer_drawSpriteExt(r, sprite, subimg, inst->x, inst->y + (float)(i * 40),
                               1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
    }
}





static void native_conveyor_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    int32_t subimg = (int32_t)inst->imageIndex;
    
    
    
    for (int32_t i = 0; (float)i < inst->imageYscale; i++) {
        float yy = inst->y + (float)(i * 20);
        for (int32_t j = 0; (float)j < inst->imageXscale; j++) {
            Renderer_drawSpriteExt(r, inst->spriteIndex, subimg,
                                   inst->x + (float)(j * 20), yy,
                                   1.0f, 1.0f, 0.0f, 0xFFFFFFu, inst->imageAlpha);
        }
    }
}









static struct {
    int32_t siner, full, ofull, firstx, secondx;
    bool ready;
} spiderCache = { .ready = false };

static void initSpiderCache(DataWin* dw) {
    spiderCache.siner   = findSelfVarId(dw, "siner");
    spiderCache.full    = findSelfVarId(dw, "full");
    spiderCache.ofull   = findSelfVarId(dw, "ofull");
    spiderCache.firstx  = findSelfVarId(dw, "firstx");
    spiderCache.secondx = findSelfVarId(dw, "secondx");
    spiderCache.ready = (spiderCache.siner >= 0 && spiderCache.full >= 0 &&
                         spiderCache.ofull >= 0 && spiderCache.firstx >= 0 &&
                         spiderCache.secondx >= 0);
}

static void native_spiderstrand_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!spiderCache.ready || runner->renderer == NULL) return;

    GMLReal siner = inst->x * 0.5;
    Instance_setSelfVar(inst, spiderCache.siner, RValue_makeReal(siner));

    GMLReal s = GMLReal_sin(siner * 0.25) * 127.0 + 127.0;
    GMLReal c = GMLReal_cos(siner * 0.25) * 127.0 + 127.0;
    uint32_t iFull = (uint32_t)(int32_t)s;  if (iFull > 255) iFull = 255;
    uint32_t iOful = (uint32_t)(int32_t)c;  if (iOful > 255) iOful = 255;
    Instance_setSelfVar(inst, spiderCache.full,  RValue_makeReal((GMLReal)iFull));
    Instance_setSelfVar(inst, spiderCache.ofull, RValue_makeReal((GMLReal)iOful));

    
    uint32_t col1 = iFull | (iFull << 8) | (iFull << 16);
    uint32_t col2 = iOful | (iOful << 8) | (iOful << 16);
    float firstx  = (float)selfReal(inst, spiderCache.firstx);
    float secondx = (float)selfReal(inst, spiderCache.secondx);

    Renderer* r = runner->renderer;
    if (r->vtable->drawLineColor != NULL) {
        r->vtable->drawLineColor(r, inst->x + firstx, 0.0f, inst->x + secondx, 159.0f,
                                 1.0f, col1, col2, 0.5f);
    } else if (r->vtable->drawLine != NULL) {
        
        uint32_t avgR = (iFull + iOful) / 2;
        uint32_t colA = avgR | (avgR << 8) | (avgR << 16);
        r->vtable->drawLine(r, inst->x + firstx, 0.0f, inst->x + secondx, 159.0f,
                            1.0f, colA, 0.5f);
    }
}









static struct {
    int32_t sn;
    bool ready;
} redpipevCache = { .ready = false };

static void initRedpipevCache(DataWin* dw) {
    redpipevCache.sn = findSelfVarId(dw, "sn");
    redpipevCache.ready = (redpipevCache.sn >= 0);
}

static void native_redpipev_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!redpipevCache.ready || runner->renderer == NULL) return;

    GMLReal sn = selfReal(inst, redpipevCache.sn) + 1.0;
    Instance_setSelfVar(inst, redpipevCache.sn, RValue_makeReal(sn));
    int32_t subimg = (int32_t)(sn * 0.25);  

    Renderer* r = runner->renderer;
    Renderer_drawSprite(r, 986, subimg, inst->x, inst->y);
    Renderer_drawSprite(r, 984, subimg, inst->x, inst->y + 16.0f + (inst->imageYscale - 1.0f) * 19.0f);
    
    for (int32_t i = 0; (float)i < inst->imageYscale; i++) {
        Renderer_drawSprite(r, 985, subimg, inst->x, inst->y + 1.0f + (float)(i * 19));
    }
}







static struct {
    int32_t siner, a, boff, coff;
    bool ready;
} lavaWaverCache = { .ready = false };

static void initLavaWaverCache(DataWin* dw) {
    lavaWaverCache.siner = findSelfVarId(dw, "siner");
    lavaWaverCache.a     = findSelfVarId(dw, "a");
    lavaWaverCache.boff  = findSelfVarId(dw, "boff");
    lavaWaverCache.coff  = findSelfVarId(dw, "coff");
    lavaWaverCache.ready = (lavaWaverCache.siner >= 0 && lavaWaverCache.a >= 0 &&
                            lavaWaverCache.boff >= 0 && lavaWaverCache.coff >= 0);
}

static void native_trueLavawaver_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!lavaWaverCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, lavaWaverCache.siner) + 1.0;
    Instance_setSelfVar(inst, lavaWaverCache.siner, RValue_makeReal(siner));

    GMLReal b = selfReal(inst, lavaWaverCache.boff);
    GMLReal c = selfReal(inst, lavaWaverCache.coff);
    GMLReal a = selfReal(inst, lavaWaverCache.a) + 3.0;

    uint32_t spriteIdx = (uint32_t)inst->spriteIndex;
    int32_t spriteW = (spriteIdx < ctx->dataWin->sprt.count) ? (int32_t)ctx->dataWin->sprt.sprites[spriteIdx].width : 0;
    int32_t subimg = (int32_t)inst->imageIndex;

    int32_t tpagIndex = Renderer_resolveTPAGIndex(ctx->dataWin, inst->spriteIndex, subimg);
    if (b != 0.0 && spriteW > 0 && tpagIndex >= 0) {
        float drawAlpha = r->drawAlpha;

        // ОПТИМИЗАЦИЯ: Математика вынесена из внутреннего цикла!
        for (int32_t i = 0; i < 40; i += 2) {
            a += 1.0;
            float base_xx = (float)(inst->x + GMLReal_sin(a / b) * c);
            float base_yy = inst->y + (float)i;

            for (int32_t g = 0; g < 4; g++) {
                float xx = base_xx + (float)(g * 100);
                for (int32_t f = 0; f < 8; f++) {
                    r->vtable->drawSpritePart(r, tpagIndex, 0, i, spriteW, 2,
                                              xx, base_yy + (float)(f * 40), 1.0f, 1.0f,
                                              0xFFFFFFu, drawAlpha);
                }
            }
        }
    }
    Instance_setSelfVar(inst, lavaWaverCache.a, RValue_makeReal(a));

    float darkAlpha = (float)(GMLReal_sin(siner / 12.0) * 0.3 + 0.5);
    if (darkAlpha < 0.0f) darkAlpha = 0.0f;
    if (darkAlpha > 1.0f) darkAlpha = 1.0f;
    if (runner->currentRoom) {
        float vx = (float)runner->currentRoom->views[0].viewX;
        float vy = (float)runner->currentRoom->views[0].viewY;
        r->vtable->drawRectangle(r, vx - 10.0f, vy - 10.0f, vx + 330.0f, vy + 250.0f,
                                 0x000000u, darkAlpha, false);
    }
    r->drawAlpha = 1.0f;
}

static struct {
    int32_t siner;
    bool ready;
} antiWaverCache = { .ready = false };

static void initAntiWaverCache(DataWin* dw) {
    antiWaverCache.siner = findSelfVarId(dw, "siner");
    antiWaverCache.ready = (antiWaverCache.siner >= 0);
}

static void native_trueAntiwaver_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!antiWaverCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, antiWaverCache.siner) + 1.0;
    Instance_setSelfVar(inst, antiWaverCache.siner, RValue_makeReal(siner));

    float basePulse = (float)(GMLReal_sin(siner / 12.0) * 0.5 + 0.5);
    float xWidth = inst->imageXscale * 20.0f;
    for (int32_t i = 0; i < 8; i++) {
        float alpha = basePulse * (1.0f - (float)i / 8.0f);
        if (alpha <= 0.0f) continue;
        if (alpha > 1.0f) alpha = 1.0f;
        float y1 = (inst->y + 16.0f) - (float)(i * 5);
        float y2 = (inst->y + 20.0f) - (float)(i * 5);
        r->vtable->drawRectangle(r, inst->x, y1, inst->x + xWidth, y2,
                                 0x000000u, alpha, false);
    }
    r->drawAlpha = 1.0f;
}




static struct {
    int32_t timer, f_l, f_d;
    bool ready;
} pipersteamCache = { .ready = false };  

static void initPiperCache(DataWin* dw) {
    pipersteamCache.timer = findSelfVarId(dw, "timer");
    pipersteamCache.f_l   = findSelfVarId(dw, "f_l");
    pipersteamCache.f_d   = findSelfVarId(dw, "f_d");
    pipersteamCache.ready = (pipersteamCache.timer >= 0 && pipersteamCache.f_l >= 0 &&
                             pipersteamCache.f_d >= 0);
}

static void native_piperBluejet_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!pipersteamCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal timer = selfReal(inst, pipersteamCache.timer) + 1.0;
    GMLReal f_l   = selfReal(inst, pipersteamCache.f_l);
    GMLReal f_d   = selfReal(inst, pipersteamCache.f_d);

    Renderer_drawSprite(r, 987, 0, inst->x, inst->y);
    Renderer_drawSprite(r, 988, 0, inst->x, inst->y + (float)(f_l * 3.0));

    if (timer == 30.0)        { f_l = 0.0; f_d = 1.0; }
    if (timer > 30.0 && timer < 50.0) {
        f_l += 0.2;
        if (f_l >= 1.0) timer = 50.0;
    }
    if (timer >= 50.0 && timer < 70.0) {
        f_l = 1.0 - GMLReal_sin(timer * 1.5) * 0.1;
    }
    if (timer >= 70.0) {
        f_l -= 0.2;
        if (f_l <= 0.0) { f_l = 0.0; f_d = 0.0; timer = 0.0; }
    }

    Instance_setSelfVar(inst, pipersteamCache.timer, RValue_makeReal(timer));
    Instance_setSelfVar(inst, pipersteamCache.f_l,   RValue_makeReal(f_l));
    Instance_setSelfVar(inst, pipersteamCache.f_d,   RValue_makeReal(f_d));

    if (f_d == 1.0) {
        float fl = (float)f_l;
        Renderer_drawSpriteExt(r, 975, 0,
                               inst->x + 7.0f, inst->y + 8.0f + fl * 3.0f,
                               0.5f + fl * 0.5f, fl, 0.0f, 0xFFFFFFu, fl);
    }
}





static void native_piperSteam_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!pipersteamCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal timer = selfReal(inst, pipersteamCache.timer) + 1.0;
    GMLReal f_l   = selfReal(inst, pipersteamCache.f_l);

    Renderer_drawSprite(r, 987, 0, inst->x, inst->y);
    Renderer_drawSprite(r, 988, 0, inst->x, inst->y + (float)(f_l * 3.0));

    if (timer == 30.0) { f_l = 0.0; }
    if (timer > 30.0 && timer < 50.0) {
        Runner_createInstance(runner, inst->x + 7.0, inst->y + 6.0 + f_l * 3.0, 58);
        f_l += 0.3;
        if (f_l >= 3.0) timer = 50.0;
    }
    if (timer >= 50.0 && timer < 90.0) {
        f_l -= 0.1;
        if (f_l <= 0.0) { f_l = 0.0; timer = 25.0; }
    }

    Instance_setSelfVar(inst, pipersteamCache.timer, RValue_makeReal(timer));
    Instance_setSelfVar(inst, pipersteamCache.f_l,   RValue_makeReal(f_l));
}





static struct {
    int32_t siner;
    bool ready;
} hotlandRedXCache = { .ready = false };

static void initHotlandRedXCache(DataWin* dw) {
    hotlandRedXCache.siner = findSelfVarId(dw, "siner");
    hotlandRedXCache.ready = (hotlandRedXCache.siner >= 0);
}

static void native_hotlandRedX_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!hotlandRedXCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, hotlandRedXCache.siner) + 1.0;
    Instance_setSelfVar(inst, hotlandRedXCache.siner, RValue_makeReal(siner));

    int32_t subimg = (int32_t)inst->imageIndex;
    
    for (int32_t i = 0; (float)i < inst->imageYscale; i++) {
        
        float xoff = (float)(GMLReal_cos((siner + (GMLReal)(i * 4)) / 12.0) * 10.0);
        
        float alpha = (float)GMLReal_sin((GMLReal)i * 0.5 - siner / 12.0);
        if (alpha < 0.0f) alpha = -alpha;
        int32_t sprite = (i == 0) ? 982 : inst->spriteIndex;
        Renderer_drawSpriteExt(r, sprite, subimg,
                               inst->x + xoff, inst->y + (float)(i * 40),
                               1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
    }
}




static struct {
    int32_t siner, alp, go, cw, w, xmode;
    bool ready;
} bottomglowerCache = { .ready = false };

static void initBottomglowerCache(DataWin* dw) {
    bottomglowerCache.siner = findSelfVarId(dw, "siner");
    bottomglowerCache.alp   = findSelfVarId(dw, "alp");
    bottomglowerCache.go    = findSelfVarId(dw, "go");
    bottomglowerCache.cw    = findSelfVarId(dw, "cw");
    bottomglowerCache.w     = findSelfVarId(dw, "w");
    bottomglowerCache.xmode = findSelfVarId(dw, "xmode");
    bottomglowerCache.ready = (bottomglowerCache.siner >= 0 && bottomglowerCache.xmode >= 0);
}

static void native_bottomglower_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!bottomglowerCache.ready || runner->renderer == NULL || !runner->currentRoom) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, bottomglowerCache.siner) + 1.0;
    Instance_setSelfVar(inst, bottomglowerCache.siner, RValue_makeReal(siner));

    float go_ = (float)GMLReal_sin(siner / 30.0);
    if (go_ < 0.0f) go_ = -go_;
    int32_t xmode = selfInt(inst, bottomglowerCache.xmode);

    float cw = 0.0f;
    float roomH = (float)runner->currentRoom->height;
    float vx = (float)runner->currentRoom->views[0].viewX;

    
    GMLReal finalAlp = 0.0, finalW = 0.0;

    for (int32_t i = 0; i < 10; i++) {
        float alp = (go_ - (float)i * 0.1f) / 1.2f;
        if (xmode == 1) alp /= 1.8f;
        if (alp < 0.0f) alp = 0.0f;

        float w = (float)(10 - i) * go_ * 1.4f;
        if (xmode == 1) w /= 1.2f;
        
        float rounded = (w >= 0.0f) ? floorf(w + 0.5f) : -floorf(-w + 0.5f);

        float bottom = roomH - cw;
        float top    = (roomH - cw - rounded) + 1.0f;

        if (alp > 0.0f) {
            
            
            r->vtable->drawRectangle(r, vx - 10.0f, bottom, vx + 330.0f, top,
                                     0x0000FFu, alp, false);
        }
        cw += rounded;
        finalAlp = alp; finalW = rounded;
    }
    
    Instance_setSelfVar(inst, bottomglowerCache.alp, RValue_makeReal(finalAlp));
    Instance_setSelfVar(inst, bottomglowerCache.go,  RValue_makeReal(go_));
    Instance_setSelfVar(inst, bottomglowerCache.cw,  RValue_makeReal(cw));
    Instance_setSelfVar(inst, bottomglowerCache.w,   RValue_makeReal(finalW));
    r->drawAlpha = 1.0f;
}




static struct {
    int32_t seg, segno, fakey, fakev, on, con, timer, shake;
    bool ready;
} counterscrollerCache = { .ready = false };

static void initCounterscrollerCache(DataWin* dw) {
    counterscrollerCache.seg    = findSelfVarId(dw, "seg");
    counterscrollerCache.segno  = findSelfVarId(dw, "segno");
    counterscrollerCache.fakey  = findSelfVarId(dw, "fakey");
    counterscrollerCache.fakev  = findSelfVarId(dw, "fakev");
    counterscrollerCache.on     = findSelfVarId(dw, "on");
    counterscrollerCache.con    = findSelfVarId(dw, "con");
    counterscrollerCache.timer  = findSelfVarId(dw, "timer");
    counterscrollerCache.shake  = findSelfVarId(dw, "shake");
    counterscrollerCache.ready = (counterscrollerCache.seg >= 0 && counterscrollerCache.segno >= 0 &&
                                  counterscrollerCache.fakey >= 0 && counterscrollerCache.fakev >= 0 &&
                                  counterscrollerCache.on >= 0 && counterscrollerCache.con >= 0 &&
                                  counterscrollerCache.timer >= 0 && counterscrollerCache.shake >= 0);
}

static void native_counterscroller_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!counterscrollerCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t seg   = selfInt(inst, counterscrollerCache.seg);
    int32_t segno = selfInt(inst, counterscrollerCache.segno);
    GMLReal fakey = selfReal(inst, counterscrollerCache.fakey);
    GMLReal fakev = selfReal(inst, counterscrollerCache.fakev);
    int32_t on    = selfInt(inst, counterscrollerCache.on);
    GMLReal con   = selfReal(inst, counterscrollerCache.con);
    GMLReal timer = selfReal(inst, counterscrollerCache.timer);
    GMLReal shake = selfReal(inst, counterscrollerCache.shake);

    
    for (int32_t i = 0; i < 20; i++) {
        int32_t j = -(seg + i);
        if (j > 0) {
            float drawY = (inst->y - 360.0f) + (float)fakey + (float)(i * 40);
            if (j < segno) {
                Renderer_drawSprite(r, 1806, 0, inst->x, drawY);
            } else if (j == segno) {
                Renderer_drawSprite(r, 1805, 0, inst->x, drawY);
            }
        }
    }

    fakey += fakev;
    if (fakey > 40.0) { fakey -= 40.0; if (on == 1) seg -= 1; }
    if (fakey < -40.0) { fakey += 40.0; if (on == 1) seg += 1; }

    if (con == 1.0) {
        fakey += 1.0;
        timer += 1.0;
        if (timer >= 17.0) { timer = 0.0; con = 1.5; }
    }
    if (con == 1.5) {
        timer += 1.0;
        if (timer > 30.0) { timer = 0.0; con = 2.0; }
    }
    if (con == 2.0) {
        
        
        double r1 = (double)rand() / (double)RAND_MAX * (double)shake;
        double r2 = (double)rand() / (double)RAND_MAX * (double)shake;
        inst->x = (float)(inst->xstart + r1 - shake / 2.0);
        inst->y = (float)(inst->ystart + r2 - shake / 2.0);
        timer += 1.0;
        if (timer > 60.0) {
            inst->x = inst->xstart; inst->y = inst->ystart;
            con = 3.0; fakev = -1.0;
        }
    }
    if (con == 3.0) {
        fakev -= 0.25;
        if (fakev <= -15.0) { fakev = -15.0; on = 0; con = 4.0; timer = 0.0; }
    }
    if (con == 4.0) {
        timer += 1.0;
        if (timer > 150.0) con = 5.0;
    }
    if (con == 5.0) { fakev = 0.0; con = 6.0; }

    Instance_setSelfVar(inst, counterscrollerCache.seg,   RValue_makeReal((GMLReal)seg));
    Instance_setSelfVar(inst, counterscrollerCache.fakey, RValue_makeReal(fakey));
    Instance_setSelfVar(inst, counterscrollerCache.fakev, RValue_makeReal(fakev));
    Instance_setSelfVar(inst, counterscrollerCache.on,    RValue_makeReal((GMLReal)on));
    Instance_setSelfVar(inst, counterscrollerCache.con,   RValue_makeReal(con));
    Instance_setSelfVar(inst, counterscrollerCache.timer, RValue_makeReal(timer));
}








static struct {
    int32_t a;
    bool ready;
} bgCoreCache = { .ready = false };

static void initBgCoreCache(DataWin* dw) {
    bgCoreCache.a = findSelfVarId(dw, "a");
    bgCoreCache.ready = (bgCoreCache.a >= 0);
}

static void native_backgrounderCore_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!bgCoreCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    r->vtable->drawRectangle(r, -10.0f, -10.0f, 2000.0f, 500.0f,
                             0x000000u, 1.0f, false);

    
    
    int32_t bgDefIdx = runner->backgrounds[0].backgroundIndex;
    int32_t tpagIndex = Renderer_resolveBackgroundTPAGIndex(ctx->dataWin, bgDefIdx);
    if (tpagIndex < 0) return;
    TexturePageItem* tpag = &ctx->dataWin->tpag.items[tpagIndex];
    int32_t bgW = (int32_t)tpag->boundingWidth;
    int32_t bgH = (int32_t)tpag->boundingHeight;

    GMLReal a = selfReal(inst, bgCoreCache.a) + 1.0;
    GMLReal b = 1.0;
    GMLReal c = 6.0;

    for (int32_t i = bgH; i > 0; i--) {
        a += 1.0;
        
        
        
        
        if (c > 0.0) {
            c -= 0.1;
            if (c < 0.0) c = 0.0;
        }
        float xoff = (float)(GMLReal_sin(a / b) * c);
        r->vtable->drawSpritePart(r, tpagIndex, 0, i, bgW, 1,
                                  inst->x + xoff, inst->y + (float)i,
                                  1.0f, 1.0f, 0xFFFFFFu, inst->imageAlpha);
    }
    Instance_setSelfVar(inst, bgCoreCache.a, RValue_makeReal(a));
}




static struct {
    int32_t siner, active, blue, ex, activebuffer;
    bool ready;
} bluelaserCache = { .ready = false };

static void initBluelaserCache(DataWin* dw) {
    bluelaserCache.siner        = findSelfVarId(dw, "siner");
    bluelaserCache.active       = findSelfVarId(dw, "active");
    bluelaserCache.blue         = findSelfVarId(dw, "blue");
    bluelaserCache.ex           = findSelfVarId(dw, "ex");
    bluelaserCache.activebuffer = findSelfVarId(dw, "activebuffer");
    bluelaserCache.ready = (bluelaserCache.siner >= 0 && bluelaserCache.active >= 0 &&
                            bluelaserCache.blue >= 0 && bluelaserCache.ex >= 0 &&
                            bluelaserCache.activebuffer >= 0);
}

static void native_bluelaser_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!bluelaserCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, bluelaserCache.siner) + 1.0;
    Instance_setSelfVar(inst, bluelaserCache.siner, RValue_makeReal(siner));

    Renderer_drawSprite(r, inst->spriteIndex, (int32_t)inst->imageIndex, inst->x, inst->y);

    int32_t active = selfInt(inst, bluelaserCache.active);
    int32_t blue   = selfInt(inst, bluelaserCache.blue);
    int32_t activebuffer = selfInt(inst, bluelaserCache.activebuffer);

    if (active != 2) {
        float alpha = (float)GMLReal_sin(siner / 3.0);
        if (alpha < 0.0f) alpha = -alpha;
        alpha = alpha * 0.5f + 0.5f;

        if (active == 0) {
            alpha = 0.3f;
            inst->imageSpeed = 0.0f;
        } else {
            inst->imageSpeed = 0.5f;
        }

        uint32_t fillColor = 0xFFFFFFu;
        if (blue == 1) {
            fillColor = 0xFFA914u;                     
            inst->spriteIndex = 1956;
        } else if (blue == 2) {
            fillColor = 0x40A0FFu;                     
            inst->spriteIndex = 1955;
        }

        
        r->vtable->drawRectangle(r, inst->x + 8.0f, inst->y + 16.0f,
                                 inst->x + 11.0f, inst->y + 320.0f,
                                 fillColor, alpha, false);

        if (active == 1 && activebuffer < 0) {
            
            BuiltinFunc collRect = VMBuiltins_find("collision_rectangle");
            if (collRect) {
                RValue args[7] = {
                    RValue_makeReal(inst->x + 9.0), RValue_makeReal(inst->y + 18.0),
                    RValue_makeReal(inst->x + 10.0), RValue_makeReal(inst->y + 320.0),
                    RValue_makeReal(1576.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
                };
                Instance* savedSelf = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = collRect(ctx, args, 7);
                ctx->currentInstance = savedSelf;
                if (RValue_toInt32(res) >= 0) {
                    Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0);
                }
                RValue_free(&res);
            }
        }
    } else {
        inst->spriteIndex = 1957;
    }

    if (selfInt(inst, bluelaserCache.ex) == 1 && findInstanceByObject(runner, 784) == NULL) {
        inst->alarm[3] = 1;
        Instance_setSelfVar(inst, bluelaserCache.ex, RValue_makeReal(0.0));
    }

    activebuffer -= 1;
    if (active != 1) activebuffer = 1;
    Instance_setSelfVar(inst, bluelaserCache.activebuffer, RValue_makeReal((GMLReal)activebuffer));

    r->drawAlpha = 1.0f;
}








static struct {
    int32_t siner, powered;
    bool ready;
} coreLightstripCache = { .ready = false };

static void initCoreLightstripCache(DataWin* dw) {
    coreLightstripCache.siner   = findSelfVarId(dw, "siner");
    coreLightstripCache.powered = findSelfVarId(dw, "powered");
    coreLightstripCache.ready = (coreLightstripCache.siner >= 0 && coreLightstripCache.powered >= 0);
}

static void native_coreLightstrip_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!coreLightstripCache.ready || runner->renderer == NULL) return;

    
    Instance* kff = findInstanceByObject(runner, 1185);
    if (kff != NULL) inst->depth = kff->depth + 1;

    GMLReal siner = selfReal(inst, coreLightstripCache.siner);
    int32_t powered = selfInt(inst, coreLightstripCache.powered);
    if (powered == 1) siner += 1.0;
    else              siner = 0.0;
    Instance_setSelfVar(inst, coreLightstripCache.siner, RValue_makeReal(siner));

    if (inst->imageXscale < 0.0f) return;
    int32_t subimg = (int32_t)(siner / 6.0);
    Renderer* r = runner->renderer;
    
    for (int32_t i = 0; (float)i < inst->imageXscale; i++) {
        Renderer_drawSprite(r, 999, subimg, inst->x + (float)(i * 20), inst->y);
    }
}




static struct {
    int32_t side;
    bool ready;
} plusbombCache = { .ready = false };

static void initPlusbombCache(DataWin* dw) {
    plusbombCache.side = findSelfVarId(dw, "side");
    plusbombCache.ready = (plusbombCache.side >= 0);
}

static void native_plusbomb_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!plusbombCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    Renderer_drawSprite(r, inst->spriteIndex, (int32_t)inst->imageIndex, inst->x, inst->y);

    int32_t side = selfInt(inst, plusbombCache.side);
    if (side == 0) return;

    
    uint32_t spriteIdx = (uint32_t)inst->spriteIndex;
    float spriteW = 0.0f;
    if (spriteIdx < ctx->dataWin->sprt.count)
        spriteW = (float)ctx->dataWin->sprt.sprites[spriteIdx].width;

    float x1, x2;
    if (side == 1) { x1 = inst->x + 2.0f + spriteW + 1.0f;  x2 = inst->x + 22.0f + spriteW + 1.0f; }
    else           { x1 = inst->x + 2.0f - spriteW - 1.0f;  x2 = inst->x + 22.0f - spriteW - 1.0f; }

    r->vtable->drawRectangle(r, x1, inst->y + 6.0f, x2, inst->y + 28.0f,
                             0xFFFFFFu, 1.0f, false);
}







static struct {
    int32_t con, myspeed, eo, myx, nowx, attacklength, shake;
    int32_t gIdealborder, gTurntimer;  
    bool ready;
} leglineCache = { .ready = false };

static void initLeglineCache(VMContext* ctx, DataWin* dw) {
    leglineCache.con          = findSelfVarId(dw, "con");
    leglineCache.myspeed      = findSelfVarId(dw, "myspeed");
    leglineCache.eo           = findSelfVarId(dw, "eo");
    leglineCache.myx          = findSelfVarId(dw, "myx");
    leglineCache.nowx         = findSelfVarId(dw, "nowx");
    leglineCache.attacklength = findSelfVarId(dw, "attacklength");
    leglineCache.shake        = findSelfVarId(dw, "shake");
    leglineCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    leglineCache.gTurntimer   = findGlobalVarId(ctx, "turntimer");
    leglineCache.ready = (leglineCache.con >= 0 && leglineCache.myspeed >= 0 &&
                          leglineCache.eo >= 0 && leglineCache.myx >= 0 &&
                          leglineCache.attacklength >= 0 && leglineCache.shake >= 0 &&
                          leglineCache.gIdealborder >= 0);
}


static GMLReal leglineGetBorder(VMContext* ctx, int32_t idx) {
    int64_t k = ((int64_t)leglineCache.gIdealborder << 32) | (uint32_t)idx;
    ptrdiff_t p = hmgeti(ctx->globalArrayMap, k);
    if (p < 0) return 0.0;
    return RValue_toReal(ctx->globalArrayMap[p].value);
}


static void leglineDrawShared(VMContext* ctx, Runner* runner, Instance* inst, bool right) {
    if (!leglineCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal con = selfReal(inst, leglineCache.con);
    GMLReal myspeed = selfReal(inst, leglineCache.myspeed);
    GMLReal eo  = selfReal(inst, leglineCache.eo);
    GMLReal myx = selfReal(inst, leglineCache.myx);
    GMLReal attackLen = selfReal(inst, leglineCache.attacklength);
    GMLReal shake = selfReal(inst, leglineCache.shake);

    GMLReal ibL = leglineGetBorder(ctx, 0);
    GMLReal ibR = leglineGetBorder(ctx, 1);
    GMLReal ibT = leglineGetBorder(ctx, 2);
    GMLReal ibB = leglineGetBorder(ctx, 3);

    
    if (con == 1.0 && myspeed > 0.0) {
        uint32_t color;
        int32_t eoi = (int32_t)eo;
        if (eoi == 0)      color = 0x0000FFu; 
        else if (eoi == 1) {
            color = 0x00FFFFu;                
            
            BuiltinFunc sp = VMBuiltins_find("snd_play");
            if (sp) { RValue a = RValue_makeReal(119.0);
                      Instance* saved = (Instance*)ctx->currentInstance;
                      ctx->currentInstance = inst;
                      RValue rr = sp(ctx, &a, 1); ctx->currentInstance = saved; RValue_free(&rr); }
        } else             color = 0x000000u; 

        float x1, x2;
        if (!right) { x1 = (float)(ibL + 6.0); x2 = (float)(ibL + attackLen - 6.0); }
        else        { x1 = (float)(ibR - 6.0); x2 = (float)(ibR - attackLen + 6.0); }
        r->vtable->drawRectangle(r, x1, (float)(ibT + 6.0), x2, (float)(ibB - 4.0),
                                 color, 1.0f, true);  
        if (!right) { x1 = (float)(ibL + 7.0); x2 = (float)(ibL + attackLen - 5.0); }
        else        { x1 = (float)(ibR - 7.0); x2 = (float)(ibR - attackLen + 5.0); }
        r->vtable->drawRectangle(r, x1, (float)(ibT + 7.0), x2, (float)(ibB - 3.0),
                                 color, 1.0f, true);  
        
        float cx = !right ? (float)(ibL + attackLen * 0.5) : (float)(ibR - attackLen * 0.5);
        Renderer_drawSprite(r, 544, eoi, cx, (float)(ibT + 30.0));
    }
    if (con == 1.0) {
        eo += 1.0;
        if (eo > 2.0) eo = 0.0;
    }

    if (con == 2.0) {
        BuiltinFunc sp = VMBuiltins_find("snd_play");
        if (sp) { RValue a = RValue_makeReal(14.0);
                  Instance* saved = (Instance*)ctx->currentInstance;
                  ctx->currentInstance = inst;
                  RValue rr = sp(ctx, &a, 1); ctx->currentInstance = saved; RValue_free(&rr); }
        if (myspeed > 0.0) {
            
            
            uint32_t spriteIdx = (uint32_t)inst->spriteIndex;
            float spriteW = 0.0f;
            if (spriteIdx < ctx->dataWin->sprt.count)
                spriteW = (float)ctx->dataWin->sprt.sprites[spriteIdx].width;
            inst->x = (float)(ibL - spriteW);
            Instance_setSelfVar(inst, leglineCache.nowx, RValue_makeReal(inst->x));
            myx = 0.0;
        }
        con = 3.0;
    }

    if (con == 3.0) {
        myx += myspeed;
        if (myx >= attackLen - myspeed) {
            myx = attackLen;
            con = 4.0;
            inst->alarm[4] = 6;
            shake = 5.0;
        }
    }

    if (shake > 0.0) shake -= 1.0;

    if (con == 5.0) {
        shake = 0.0;
        myx -= myspeed;
        if (myx <= 0.0) { Runner_destroyInstance(runner, inst);  }
    }

    
    Instance_setSelfVar(inst, leglineCache.con,   RValue_makeReal(con));
    Instance_setSelfVar(inst, leglineCache.eo,    RValue_makeReal(eo));
    Instance_setSelfVar(inst, leglineCache.myx,   RValue_makeReal(myx));
    Instance_setSelfVar(inst, leglineCache.shake, RValue_makeReal(shake));

    
    if (con >= 3.0) {
        uint32_t spriteIdx = (uint32_t)inst->spriteIndex;
        float spriteH = 0.0f;
        if (spriteIdx < ctx->dataWin->sprt.count)
            spriteH = (float)ctx->dataWin->sprt.sprites[spriteIdx].height;
        int32_t subimg = (int32_t)inst->imageIndex;
        int32_t tpagIndex = Renderer_resolveTPAGIndex(ctx->dataWin, inst->spriteIndex, subimg);

        BuiltinFunc collRect = VMBuiltins_find("collision_rectangle");
        BuiltinFunc collLine = VMBuiltins_find("collision_line");

        for (int32_t i = 0; i < 5; i++) {
            
            float rr = (float)(((double)rand()/(double)RAND_MAX - (double)rand()/(double)RAND_MAX) * shake);
            float rowY = (float)(ibT + 5.0 + (double)(i * 30) + rr);

            if (tpagIndex >= 0) {
                
                
                uint32_t spriteW = 0;
                if (spriteIdx < ctx->dataWin->sprt.count)
                    spriteW = ctx->dataWin->sprt.sprites[spriteIdx].width;
                int32_t srcX = right ? 0 : (int32_t)((float)spriteW - (float)myx + rr);
                int32_t srcW = (int32_t)((float)myx + rr);
                float dstX = right ? (float)(ibR - myx) : (float)ibL;
                if (srcW > 0) {
                    r->vtable->drawSpritePart(r, tpagIndex, srcX, 0, srcW, (int32_t)spriteH,
                                              dstX, rowY, 1.0f, 1.0f, 0xFFFFFFu, r->drawAlpha);
                }
            }

            
            if (collRect) {
                float cx1, cx2;
                if (!right) { cx1 = (float)ibL;                    cx2 = (float)(ibL + myx - 30.0); }
                else        { cx1 = (float)ibR;                    cx2 = (float)(ibR - myx + 30.0); }
                RValue args[7] = {
                    RValue_makeReal(cx1), RValue_makeReal(ibT + 9.0 + (double)(i * 30)),
                    RValue_makeReal(cx2), RValue_makeReal(ibT + 18.0 + (double)(i * 30)),
                    RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
                };
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = collRect(ctx, args, 7);
                ctx->currentInstance = saved;
                if (RValue_toInt32(res) >= 0) Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
                RValue_free(&res);
            }
            
            
            if (collLine) {
                float lx1, lx2;
                if (!right) { lx1 = (float)(ibL + myx - 30.0); lx2 = (float)(ibL + myx - 8.0); }
                else        { lx1 = (float)(ibR - myx + 30.0); lx2 = (float)(ibR - myx + 8.0); }
                RValue args[7] = {
                    RValue_makeReal(lx1), RValue_makeReal(ibT + 9.0 + (double)(i * 30)),
                    RValue_makeReal(lx2), RValue_makeReal(ibT + 9.0 + (double)(i * 30)),
                    RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
                };
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = collLine(ctx, args, 7);
                ctx->currentInstance = saved;
                bool hit = (RValue_toInt32(res) >= 0);
                RValue_free(&res);
                if (!hit) {
                    RValue args2[7] = {
                        RValue_makeReal(lx1), RValue_makeReal(ibT + 23.0 + (double)(i * 30)),
                        RValue_makeReal(lx2), RValue_makeReal(ibT + 9.0 + (double)(i * 30)),
                        RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
                    };
                    saved = (Instance*)ctx->currentInstance;
                    ctx->currentInstance = inst;
                    res = collLine(ctx, args2, 7);
                    ctx->currentInstance = saved;
                    hit = (RValue_toInt32(res) >= 0);
                    RValue_free(&res);
                }
                if (hit) Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
            }
        }
    }
    (void)ibB;  
}

static void native_legline_l_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    leglineDrawShared(ctx, runner, inst, false);
}

static void native_legline_r_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    leglineDrawShared(ctx, runner, inst, true);
}









static struct {
    int32_t noleg, legr, legl, arml, armr;
    int32_t legrsprite, leglsprite, armlsprite, armrsprite;
    int32_t xoffr, yoffr, legrh, xoffl, yoffl, leglh;
    int32_t sineron, siner, rsin, lsin, legh, offangle;
    int32_t dsf, ds1, ds2;
    int32_t myblend, myalpha;
    int32_t bodyopen, bodyimg;
    int32_t heartdead, endface, hurt, hurtface, face_set, faceno;
    int32_t pause, legmode;
    int32_t fadewhite, whiteval, do_room_goto, noarm;
    int32_t gFaceemotion, gFlag, gIdealborder;
    int32_t objUborder;
    bool ready;
} mettbCache = { .ready = false };

static void initMettbCache(VMContext* ctx, DataWin* dw) {
    mettbCache.noleg      = findSelfVarId(dw, "noleg");
    mettbCache.legr       = findSelfVarId(dw, "legr");
    mettbCache.legl       = findSelfVarId(dw, "legl");
    mettbCache.arml       = findSelfVarId(dw, "arml");
    mettbCache.armr       = findSelfVarId(dw, "armr");
    mettbCache.legrsprite = findSelfVarId(dw, "legrsprite");
    mettbCache.leglsprite = findSelfVarId(dw, "leglsprite");
    mettbCache.armlsprite = findSelfVarId(dw, "armlsprite");
    mettbCache.armrsprite = findSelfVarId(dw, "armrsprite");
    mettbCache.xoffr      = findSelfVarId(dw, "xoffr");
    mettbCache.yoffr      = findSelfVarId(dw, "yoffr");
    mettbCache.legrh      = findSelfVarId(dw, "legrh");
    mettbCache.xoffl      = findSelfVarId(dw, "xoffl");
    mettbCache.yoffl      = findSelfVarId(dw, "yoffl");
    mettbCache.leglh      = findSelfVarId(dw, "leglh");
    mettbCache.sineron    = findSelfVarId(dw, "sineron");
    mettbCache.siner      = findSelfVarId(dw, "siner");
    mettbCache.rsin       = findSelfVarId(dw, "rsin");
    mettbCache.lsin       = findSelfVarId(dw, "lsin");
    mettbCache.legh       = findSelfVarId(dw, "legh");
    mettbCache.offangle   = findSelfVarId(dw, "offangle");
    mettbCache.dsf        = findSelfVarId(dw, "dsf");
    mettbCache.ds1        = findSelfVarId(dw, "ds1");
    mettbCache.ds2        = findSelfVarId(dw, "ds2");
    mettbCache.myblend    = findSelfVarId(dw, "myblend");
    mettbCache.myalpha    = findSelfVarId(dw, "myalpha");
    mettbCache.bodyopen   = findSelfVarId(dw, "bodyopen");
    mettbCache.bodyimg    = findSelfVarId(dw, "bodyimg");
    mettbCache.heartdead  = findSelfVarId(dw, "heartdead");
    mettbCache.endface    = findSelfVarId(dw, "endface");
    mettbCache.hurt       = findSelfVarId(dw, "hurt");
    mettbCache.hurtface   = findSelfVarId(dw, "hurtface");
    mettbCache.face_set   = findSelfVarId(dw, "face_set");
    mettbCache.faceno     = findSelfVarId(dw, "faceno");
    mettbCache.pause      = findSelfVarId(dw, "pause");
    mettbCache.legmode    = findSelfVarId(dw, "legmode");
    mettbCache.fadewhite  = findSelfVarId(dw, "fadewhite");
    mettbCache.whiteval   = findSelfVarId(dw, "whiteval");
    mettbCache.do_room_goto = findSelfVarId(dw, "do_room_goto");
    mettbCache.noarm      = findSelfVarId(dw, "noarm");
    mettbCache.gFaceemotion = findGlobalVarId(ctx, "faceemotion");
    mettbCache.gFlag      = findGlobalVarId(ctx, "flag");
    mettbCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    mettbCache.objUborder = findObjectIndex(dw, "obj_uborder");
    mettbCache.ready = (mettbCache.noleg >= 0 && mettbCache.legr >= 0 && mettbCache.legl >= 0 &&
                        mettbCache.arml >= 0 && mettbCache.armr >= 0 &&
                        mettbCache.siner >= 0 && mettbCache.legh >= 0 && mettbCache.myblend >= 0 &&
                        mettbCache.myalpha >= 0 && mettbCache.bodyimg >= 0 &&
                        mettbCache.hurt >= 0 && mettbCache.pause >= 0 && mettbCache.sineron >= 0);
}


static void mettbLegParams(int32_t legState, int32_t side ,
                           int32_t* spr, float* xoff, float* yoff, int32_t* lh) {
    
    
    
    switch (legState) {
        case 0:  *spr=520; *xoff=-14; *yoff=10; *lh=36; break;
        case 1:  *spr=521; *xoff=-16; *yoff=6;  *lh=8;  break;
        case 2:  *spr=528; *xoff=-10; *yoff=14; *lh=60; break;
        case 3:  *spr=526; *xoff=-10; *yoff=14; *lh=30; break;
        case 4:  *spr=527; *xoff=-18; *yoff=2;  *lh=42; break;
        case 9:
            if (side == 0) { *spr=524; *xoff=20; *yoff=6; *lh=8; }
            else           { *spr=522; *xoff=-5; *yoff=2; *lh=6; }
            break;
        case 10:
            if (side == 0) { *spr=525; *xoff=15; *yoff=2; *lh=0; }
            else           { *spr=523; *xoff=0;  *yoff=5; *lh=2; }
            break;
        default: break;  
    }
}

static int32_t mettbArmSprite(int32_t armState) {
    switch (armState) {
        case 0: return 529; case 1: return 530; case 2: return 531; case 3: return 532;
        case 4: return 533; case 5: return 534; case 6: return 535; case 7: return 536;
        default: return 529;
    }
}

static void native_mettbBody_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!mettbCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t noleg = selfInt(inst, mettbCache.noleg);
    int32_t legr  = selfInt(inst, mettbCache.legr);
    int32_t legl  = selfInt(inst, mettbCache.legl);
    int32_t arml  = selfInt(inst, mettbCache.arml);
    int32_t armr  = selfInt(inst, mettbCache.armr);
    int32_t legrsprite = selfInt(inst, mettbCache.legrsprite);
    int32_t leglsprite = selfInt(inst, mettbCache.leglsprite);
    int32_t armlsprite = selfInt(inst, mettbCache.armlsprite);
    int32_t armrsprite = selfInt(inst, mettbCache.armrsprite);
    float xoffr = (float)selfReal(inst, mettbCache.xoffr);
    float yoffr = (float)selfReal(inst, mettbCache.yoffr);
    int32_t legrh = selfInt(inst, mettbCache.legrh);
    float xoffl = (float)selfReal(inst, mettbCache.xoffl);
    float yoffl = (float)selfReal(inst, mettbCache.yoffl);
    int32_t leglh = selfInt(inst, mettbCache.leglh);
    int32_t sineron = selfInt(inst, mettbCache.sineron);
    GMLReal siner = selfReal(inst, mettbCache.siner);
    GMLReal rsin = selfReal(inst, mettbCache.rsin);
    GMLReal lsin = selfReal(inst, mettbCache.lsin);
    int32_t legh = selfInt(inst, mettbCache.legh);
    int32_t offangle = 0;
    float dsf = (float)selfReal(inst, mettbCache.dsf);
    float myalpha = (float)selfReal(inst, mettbCache.myalpha);
    uint32_t myblend = (uint32_t)selfInt(inst, mettbCache.myblend);
    int32_t bodyopen = selfInt(inst, mettbCache.bodyopen);
    GMLReal bodyimg = selfReal(inst, mettbCache.bodyimg);
    int32_t heartdead = selfInt(inst, mettbCache.heartdead);
    int32_t endface   = selfInt(inst, mettbCache.endface);
    int32_t hurt      = selfInt(inst, mettbCache.hurt);
    int32_t hurtface  = selfInt(inst, mettbCache.hurtface);
    int32_t face_set  = selfInt(inst, mettbCache.face_set);
    int32_t faceno    = selfInt(inst, mettbCache.faceno);
    int32_t pause     = selfInt(inst, mettbCache.pause);
    int32_t legmode   = selfInt(inst, mettbCache.legmode);
    int32_t fadewhite = selfInt(inst, mettbCache.fadewhite);
    float whiteval    = (float)selfReal(inst, mettbCache.whiteval);
    int32_t noarm     = selfInt(inst, mettbCache.noarm);

    
    
    
    if (noleg == 0) {
        int32_t lh = legrh;
        mettbLegParams(legr, 0, &legrsprite, &xoffr, &yoffr, &lh);
        legrh = lh;
        lh = leglh;
        mettbLegParams(legl, 1, &leglsprite, &xoffl, &yoffl, &lh);
        leglh = lh;
    }
    
    armlsprite = mettbArmSprite(arml);
    armrsprite = mettbArmSprite(armr);

    
    if (legl != 9 && legr != 9 && legl != 10 && legr != 10) {
        offangle = 0;
        if (leglh > 10 || (legrh > 10 && sineron == 1)) siner += 1.0;
        if (sineron == 1) { rsin += 1.0; lsin += 1.0; }
        if (leglh > legrh) { legh = leglh * 2; lsin = 0.0; }
        else               { legh = legrh * 2; rsin = 0.0; }
        if (abs(leglh - legrh) < 5) { lsin = 0.0; rsin = 0.0; }
    } else {
        if (leglh > legrh) legh = leglh * 2;
        else               legh = legrh * 2;
        if (legl == 10) lsin = 0.0;
        if (sineron == 1) { siner += 1.0; rsin += 1.0; lsin += 1.0; }
        offangle = 10;
    }

    
    float ds1 = (float)((double)rand()/(double)RAND_MAX * 2.0 - 1.0) * dsf;
    float ds2 = (float)((double)rand()/(double)RAND_MAX * 2.0 - 1.0) * dsf;

    
    float sinSiner2 = (float)GMLReal_sin(siner * 0.5);           
    float sinSiner35 = (float)GMLReal_sin(siner / 3.5);          
    float cosSiner35 = (float)GMLReal_cos(siner / 3.5);          

    
    if (noleg == 0) {
        
        float rx = inst->x + 90.0f + xoffr;
        float ry = inst->y + 120.0f + yoffr - (float)legh - sinSiner2 * 0.05f;
        float ryscale = 2.0f - sinSiner35 * 0.05f;
        float rot_r = (float)GMLReal_sin(rsin / 7.0) * 10.0f - (float)offangle;
        Renderer_drawSpriteExt(r, legrsprite, 0, rx, ry, 2.0f, ryscale, rot_r, myblend, myalpha);

        
        float lx = (inst->x + 90.0f) - xoffl - 32.0f;
        float ly = inst->y + 120.0f + yoffl - (float)legh - sinSiner2 * 0.05f;
        float rot_l = (float)GMLReal_sin(lsin / 7.0) * 10.0f;
        Renderer_drawSpriteExt(r, leglsprite, 0, lx, ly, -2.0f, ryscale, rot_l, myblend, myalpha);
    }

    
    if (noarm == 0) {
        float armY = inst->y - (float)legh + 80.0f + cosSiner35 * 2.0f;
        if (arml != 5) {
            Renderer_drawSpriteExt(r, armlsprite, 0, inst->x + 36.0f + sinSiner35, armY,
                                   2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
        if (armr != 5) {
            Renderer_drawSpriteExt(r, armrsprite, 0, inst->x + 110.0f + sinSiner35, armY,
                                   -2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
    }

    
    if (bodyopen == 1) { if (bodyimg < 5.0) bodyimg += 0.25; }
    else if (bodyimg > 0.0) bodyimg -= 0.25;

    
    float bodyCX = inst->x + 72.0f + sinSiner35 + ds1;
    float bodyY  = inst->y - (float)legh + 134.0f + cosSiner35 * 2.0f + ds2;
    Renderer_drawSpriteExt(r, 579, (int32_t)bodyimg, bodyCX, bodyY,
                           2.0f, 2.0f, 0.0f, myblend, myalpha);

    
    if (findInstanceByObject(runner, 421) == NULL &&
        findInstanceByObject(runner, 450) == NULL && heartdead == 0) {
        Renderer_drawSpriteExt(r, 580, 0, bodyCX + 66.0f, bodyY + 108.0f,
                               2.0f, 2.0f, 0.0f, myblend, myalpha);
    }

    
    int32_t faceEmotion = 0;
    if (mettbCache.gFaceemotion >= 0)
        faceEmotion = (int32_t)globalReal(ctx, mettbCache.gFaceemotion);

    if (endface == 0) {
        float faceY = (inst->y + 40.0f) - (float)legh + cosSiner35 * 3.0f;
        if (hurt == 0 && face_set == 0) {
            Renderer_drawSpriteExt(r, 516, faceno, inst->x + 68.0f, faceY,
                                   2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
        if (hurt == 0 && face_set == 1) {
            Renderer_drawSpriteExt(r, 519, faceEmotion, (inst->x + 68.0f) - ds1, faceY - ds2,
                                   2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
        if (hurt == 1) {
            Renderer_drawSpriteExt(r, 518, hurtface, inst->x + 68.0f, faceY,
                                   2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
        if (hurt == 2) {
            Renderer_drawSpriteExt(r, 519, faceEmotion, (inst->x + 68.0f) - ds1, faceY - ds2,
                                   2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
    } else {
        float faceY = (inst->y + 40.0f) - (float)legh + cosSiner35 * 3.0f;
        Renderer_drawSpriteExt(r, 517, faceEmotion, inst->x + 68.0f, faceY,
                               2.0f, 2.0f, 0.0f, myblend, myalpha);
    }

    
    if (noarm == 0) {
        float armY = inst->y - (float)legh + 80.0f + cosSiner35 * 2.0f;
        if (arml == 5) {
            Renderer_drawSpriteExt(r, armlsprite, 0, inst->x + 42.0f + sinSiner35, armY,
                                   2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
        if (armr == 5) {
            Renderer_drawSpriteExt(r, armrsprite, 0, inst->x + 110.0f + sinSiner35, armY,
                                   -2.0f, 2.0f, 0.0f, myblend, myalpha);
        }
    }

    
    if (pause == 1 && hurt == 0) { hurt = 1; hurtface = (rand() & 1) ? 1 : 0; }
    if (pause == 2 && hurt == 0) { hurt = 1; hurtface = 2; }
    if (pause == 0) hurt = 0;

    
    if (sineron == 1) {
        Instance* uborder = (mettbCache.objUborder >= 0) ? findInstanceByObject(runner, mettbCache.objUborder) : NULL;
        if (uborder != NULL) inst->y = uborder->y - 136.0f;

        
        GMLReal ib2 = 0.0;
        if (mettbCache.gIdealborder >= 0) {
            int64_t k = ((int64_t)mettbCache.gIdealborder << 32) | (uint32_t)2;
            ptrdiff_t p = hmgeti(ctx->globalArrayMap, k);
            if (p >= 0) ib2 = RValue_toReal(ctx->globalArrayMap[p].value);
        }
        if (ib2 < 250.0) {
            if (legmode == 0) {
                inst->depth = 0;
                legmode = 1;
                legl = (rand() & 1) ? 10 : 9;
                legr = legl;
            }
        } else if (legmode == 1) {
            inst->depth = 10;
            legmode = 0;
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0);
        }
    }

    
    if (fadewhite == 1) {
        inst->depth = -999999;
        whiteval += 0.2f;
        float wa = whiteval; if (wa > 1.0f) wa = 1.0f; if (wa < 0.0f) wa = 0.0f;
        
        r->vtable->drawRectangle(r, -10.0f, -10.0f, 999.0f, 999.0f, 0xFFFFFFu, wa, false);
        if (whiteval > 10.0f) {
            float a2 = -1.0f + whiteval / 10.0f;
            if (a2 > 1.0f) a2 = 1.0f; if (a2 < 0.0f) a2 = 0.0f;
            r->vtable->drawRectangle(r, -10.0f, -10.0f, 999.0f, 999.0f, 0x000000u, a2, false);
        }
        if (whiteval == 10.0f) {
            
            GMLReal fv = 0.0;
            if (mettbCache.gFlag >= 0) {
                int64_t k = ((int64_t)mettbCache.gFlag << 32) | (uint32_t)425;
                ptrdiff_t p = hmgeti(ctx->globalArrayMap, k);
                if (p >= 0) fv = RValue_toReal(ctx->globalArrayMap[p].value);
            }
            if (fv == 1.0) {
                BuiltinFunc sp = VMBuiltins_find("snd_play");
                if (sp) { RValue aa = RValue_makeReal(92.0);
                          Instance* saved = (Instance*)ctx->currentInstance;
                          ctx->currentInstance = inst;
                          RValue rr = sp(ctx, &aa, 1); ctx->currentInstance = saved; RValue_free(&rr); }
            }
        }
        r->drawAlpha = 1.0f;
        if (whiteval >= 44.0f) {
            Runner_createInstance(runner, 0.0, 0.0, 149);
            Instance_setSelfVar(inst, mettbCache.do_room_goto, RValue_makeReal(1.0));
        }
    }

    
    if (noleg == 1) {
        legrh = (legrh > 6) ? legrh - 4 : 6;
        leglh = (leglh > 6) ? leglh - 4 : 6;
        legh  = (legh  > 6) ? legh  - 4 : 6;
    }

    
    Instance_setSelfVar(inst, mettbCache.legrsprite, RValue_makeReal((GMLReal)legrsprite));
    Instance_setSelfVar(inst, mettbCache.leglsprite, RValue_makeReal((GMLReal)leglsprite));
    Instance_setSelfVar(inst, mettbCache.armlsprite, RValue_makeReal((GMLReal)armlsprite));
    Instance_setSelfVar(inst, mettbCache.armrsprite, RValue_makeReal((GMLReal)armrsprite));
    Instance_setSelfVar(inst, mettbCache.xoffr, RValue_makeReal(xoffr));
    Instance_setSelfVar(inst, mettbCache.yoffr, RValue_makeReal(yoffr));
    Instance_setSelfVar(inst, mettbCache.legrh, RValue_makeReal((GMLReal)legrh));
    Instance_setSelfVar(inst, mettbCache.xoffl, RValue_makeReal(xoffl));
    Instance_setSelfVar(inst, mettbCache.yoffl, RValue_makeReal(yoffl));
    Instance_setSelfVar(inst, mettbCache.leglh, RValue_makeReal((GMLReal)leglh));
    Instance_setSelfVar(inst, mettbCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, mettbCache.rsin,  RValue_makeReal(rsin));
    Instance_setSelfVar(inst, mettbCache.lsin,  RValue_makeReal(lsin));
    Instance_setSelfVar(inst, mettbCache.legh,  RValue_makeReal((GMLReal)legh));
    Instance_setSelfVar(inst, mettbCache.offangle, RValue_makeReal((GMLReal)offangle));
    Instance_setSelfVar(inst, mettbCache.ds1, RValue_makeReal(ds1));
    Instance_setSelfVar(inst, mettbCache.ds2, RValue_makeReal(ds2));
    Instance_setSelfVar(inst, mettbCache.bodyimg, RValue_makeReal(bodyimg));
    Instance_setSelfVar(inst, mettbCache.hurt, RValue_makeReal((GMLReal)hurt));
    Instance_setSelfVar(inst, mettbCache.hurtface, RValue_makeReal((GMLReal)hurtface));
    Instance_setSelfVar(inst, mettbCache.legmode, RValue_makeReal((GMLReal)legmode));
    Instance_setSelfVar(inst, mettbCache.legl, RValue_makeReal((GMLReal)legl));
    Instance_setSelfVar(inst, mettbCache.legr, RValue_makeReal((GMLReal)legr));
    Instance_setSelfVar(inst, mettbCache.whiteval, RValue_makeReal(whiteval));
}













static struct {
    int32_t siner, active, rq, rq_v, rq_s, rpy, rp, ratingsy;
    int32_t checkhp, curtype, boastmode, heel, o_ob;
    int32_t novel_armor, o_o, timeloss;
    int32_t gLanguage, gRatings, gHp, gTurntimer, gMnfight, gMyfight;
    int32_t objMettatonex;  
    int32_t mettxTurns;     
    bool ready;
} ratingsCache = { .ready = false };

static void initRatingsCache(VMContext* ctx, DataWin* dw) {
    ratingsCache.siner       = findSelfVarId(dw, "siner");
    ratingsCache.active      = findSelfVarId(dw, "active");
    ratingsCache.rq          = findSelfVarId(dw, "rq");
    ratingsCache.rq_v        = findSelfVarId(dw, "rq_v");
    ratingsCache.rq_s        = findSelfVarId(dw, "rq_s");
    ratingsCache.rpy         = findSelfVarId(dw, "rpy");
    ratingsCache.rp          = findSelfVarId(dw, "rp");
    ratingsCache.ratingsy    = findSelfVarId(dw, "ratingsy");
    ratingsCache.checkhp     = findSelfVarId(dw, "checkhp");
    ratingsCache.curtype     = findSelfVarId(dw, "curtype");
    ratingsCache.boastmode   = findSelfVarId(dw, "boastmode");
    ratingsCache.heel        = findSelfVarId(dw, "heel");
    ratingsCache.o_ob        = findSelfVarId(dw, "o_ob");
    ratingsCache.novel_armor = findSelfVarId(dw, "novel_armor");
    ratingsCache.o_o         = findSelfVarId(dw, "o_o");
    ratingsCache.timeloss    = findSelfVarId(dw, "timeloss");
    ratingsCache.gLanguage   = findGlobalVarId(ctx, "language");
    ratingsCache.gRatings    = findGlobalVarId(ctx, "ratings");
    ratingsCache.gHp         = findGlobalVarId(ctx, "hp");
    ratingsCache.gTurntimer  = findGlobalVarId(ctx, "turntimer");
    ratingsCache.gMnfight    = findGlobalVarId(ctx, "mnfight");
    ratingsCache.gMyfight    = findGlobalVarId(ctx, "myfight");
    ratingsCache.objMettatonex = 404;
    ratingsCache.mettxTurns  = findSelfVarId(dw, "turns");
    ratingsCache.ready = (ratingsCache.siner >= 0 && ratingsCache.active >= 0 &&
                          ratingsCache.rq >= 0 && ratingsCache.rq_v >= 0 &&
                          ratingsCache.rq_s >= 0 && ratingsCache.gRatings >= 0);
}


static bool ratingsIsJa(VMContext* ctx) {
    if (ratingsCache.gLanguage < 0) return false;
    RValue langVal = ctx->globalVars[ratingsCache.gLanguage];
    return (langVal.type == RVALUE_STRING && langVal.string && strcmp(langVal.string, "ja") == 0);
}


static inline void ratingsDrawText(Renderer* r, const char* text, float x, float y,
                                   float xs, float ys, float rot,
                                   uint32_t color, float alpha) {
    uint32_t savedC = r->drawColor; float savedA = r->drawAlpha;
    r->drawColor = color; r->drawAlpha = alpha;
    r->vtable->drawText(r, text, x, y, xs, ys, rot);
    r->drawColor = savedC; r->drawAlpha = savedA;
}

static void native_ratingsmaster_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!ratingsCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    bool isJa = ratingsIsJa(ctx);
    r->drawFont = isJa ? 14 : 2;

    GMLReal siner = selfReal(inst, ratingsCache.siner) + 1.0;
    Instance_setSelfVar(inst, ratingsCache.siner, RValue_makeReal(siner));

    int32_t active = selfInt(inst, ratingsCache.active);
    GMLReal ratings = globalReal(ctx, ratingsCache.gRatings);

    if (active == 1) {
        float xx = inst->x + 20.0f;
        if (isJa) xx -= 30.0f;

        float sinS4 = (float)GMLReal_sin(siner * 0.25);
        float cosS4 = (float)GMLReal_cos(siner * 0.25);

        
        {
            char numBuf[32];
            snprintf(numBuf, sizeof(numBuf), "%.16g", (double)ratings);
            BuiltinFunc gt = VMBuiltins_find("scr_gettext");
            char* titleStr = NULL;
            if (gt) {
                RValue gtArgs[2] = {
                    RValue_makeString("mett_ratings"),
                    RValue_makeString(numBuf)
                };
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = gt(ctx, gtArgs, 2);
                ctx->currentInstance = saved;
                if (res.type == RVALUE_STRING && res.string) {
                    titleStr = safeStrdup(res.string);
                }
                RValue_free(&res);
            }
            if (titleStr) {
                ratingsDrawText(r, titleStr,
                                xx + sinS4, inst->y + cosS4,
                                2.0f - sinS4 * 0.05f, 2.0f - cosS4 * 0.05f, 0.0f,
                                0xFFFFFFu, 1.0f);
                free(titleStr);
            }
        }

        
        BuiltinFunc stringWidthFn = VMBuiltins_find("string_width");
        for (int32_t i = 0; i < 6; i++) {
            
            RValue rqvVal = selfArrayGet(inst, ratingsCache.rq_v, i);
            GMLReal rqv = RValue_toReal(rqvVal);
            RValue_free(&rqvVal);

            
            char thisv[32];
            uint32_t valueColor;
            if (rqv >= 0.0) {
                
                snprintf(thisv, sizeof(thisv), "+%.16g", (double)rqv);
                valueColor = 0x00FF00u;
            } else {
                snprintf(thisv, sizeof(thisv), "%.16g", (double)rqv);
                valueColor = 0x0000FFu;  
            }

            
            RValue rqsVal = selfArrayGet(inst, ratingsCache.rq_s, i);
            GMLReal rqs = RValue_toReal(rqsVal) + (GMLReal)((i + 2) / 2);
            RValue_free(&rqsVal);
            {
                
                int64_t k = ((int64_t)ratingsCache.rq_s << 32) | (uint32_t)i;
                RValue nv = RValue_makeReal(rqs);
                ptrdiff_t p = hmgeti(inst->selfArrayMap, k);
                if (p >= 0) { RValue_free(&inst->selfArrayMap[p].value); inst->selfArrayMap[p].value = nv; }
                else        { ArrayMapEntry e = { .key = k, .value = nv }; hmputs(inst->selfArrayMap, e); }
            }

            float rowAlpha = 1.0f;
            if (rqs > 120.0) {
                rowAlpha = (float)((170.0 - rqs) / 50.0);
                if (rowAlpha < 0.0f) rowAlpha = 0.0f;
                if (rowAlpha > 1.0f) rowAlpha = 1.0f;
            }

            int32_t slim, vpos, linespace;
            if (isJa) { slim = 40; vpos = 150; linespace = 15; }
            else      { slim = 60; vpos = 130; linespace = 12; }

            
            RValue rqStrVal = selfArrayGet(inst, ratingsCache.rq, i);
            const char* rqStr = (rqStrVal.type == RVALUE_STRING && rqStrVal.string) ? rqStrVal.string : "";

            float scaleX = 1.0f;
            GMLReal swidth = 0.0;
            if (stringWidthFn) {
                RValue swArg[1] = { rqStrVal };
                RValue sw = stringWidthFn(ctx, swArg, 1);
                swidth = RValue_toReal(sw);
                RValue_free(&sw);
            }
            if (swidth > (GMLReal)(vpos - slim)) scaleX = (float)((vpos - slim) / swidth);

            int32_t spos = (int32_t)(((GMLReal)vpos - swidth * scaleX) + 0.5);  
            float xoffRow = 0.0f;
            if (rqs < 10.0) {
                xoffRow = (float)(GMLReal_cos(rqs) * 21.0 / (rqs * 2.0 + 1.0));
            }

            float yRow = inst->y + 140.0f + (float)(i * linespace);
            ratingsDrawText(r, rqStr,
                            inst->x + (float)spos + xoffRow, yRow,
                            scaleX, 1.0f, 0.0f, valueColor, rowAlpha);
            ratingsDrawText(r, thisv,
                            inst->x + (float)vpos + xoffRow, yRow,
                            1.0f, 1.0f, 0.0f, valueColor, rowAlpha);
            RValue_free(&rqStrVal);
        }

        
        
        r->vtable->drawLine(r, inst->x + 10.0f, inst->y + 40.0f,
                            inst->x + 10.0f, inst->y + 130.0f,
                            3.0f, 0xFFFFFFu, 1.0f);
        
        r->vtable->drawLine(r, inst->x + 10.0f, inst->y + 130.0f,
                            inst->x + 180.0f, inst->y + 130.0f,
                            3.0f, 0xFFFFFFu, 1.0f);
        
        r->vtable->drawLine(r, inst->x + 10.0f, inst->y + 55.0f,
                            inst->x + 180.0f, inst->y + 55.0f,
                            1.0f, 0x00FFFFu, 1.0f);

        
        GMLReal ratingsy = ratings * 0.0075;
        Instance_setSelfVar(inst, ratingsCache.ratingsy, RValue_makeReal(ratingsy));
        
        r->vtable->drawLine(r,
                            inst->x + 10.0f,  inst->y + 130.0f - (float)ratingsy,
                            inst->x + 180.0f, inst->y + 130.0f - (float)ratingsy,
                            1.0f, 0xFFFF00u, 1.0f);

        
        for (int32_t i = 0; i < 9; i++) {
            RValue rpiV  = selfArrayGet(inst, ratingsCache.rp, i);
            RValue rpi1V = selfArrayGet(inst, ratingsCache.rp, i + 1);
            GMLReal py0 = RValue_toReal(rpiV)  * 0.0075;
            GMLReal py1 = RValue_toReal(rpi1V) * 0.0075;
            RValue_free(&rpiV); RValue_free(&rpi1V);

            
            for (int32_t k = 0; k < 2; k++) {
                int32_t idx = i + k;
                GMLReal v = (k == 0) ? py0 : py1;
                int64_t key = ((int64_t)ratingsCache.rpy << 32) | (uint32_t)idx;
                RValue nv = RValue_makeReal(v);
                ptrdiff_t p = hmgeti(inst->selfArrayMap, key);
                if (p >= 0) { RValue_free(&inst->selfArrayMap[p].value); inst->selfArrayMap[p].value = nv; }
                else        { ArrayMapEntry e = { .key = key, .value = nv }; hmputs(inst->selfArrayMap, e); }
            }

            
            r->vtable->drawLine(r,
                                inst->x + 10.0f + (float)(i * 20),
                                inst->y + 130.0f - (float)py0,
                                inst->x + 30.0f + (float)(i * 20),
                                inst->y + 130.0f - (float)py1,
                                2.0f, 0xFF00FFu, 1.0f);
        }
    }

    
    GMLReal checkhp = selfReal(inst, ratingsCache.checkhp);
    GMLReal hp = globalReal(ctx, ratingsCache.gHp);
    int32_t boastmode = selfInt(inst, ratingsCache.boastmode);
    int32_t heel      = selfInt(inst, ratingsCache.heel);

    if (checkhp > hp) {
        GMLReal curtype = 1.0;
        if (boastmode == 1) { curtype = 2.0; boastmode = 0; }
        if (heel == 1) curtype = 3.0;
        Instance_setSelfVar(inst, ratingsCache.curtype, RValue_makeReal(curtype));
        Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0);
    }
    Instance_setSelfVar(inst, ratingsCache.checkhp, RValue_makeReal(hp));

    if (boastmode == 1) {
        GMLReal turntimer = (ratingsCache.gTurntimer >= 0) ? globalReal(ctx, ratingsCache.gTurntimer) : 0.0;
        GMLReal mnfight   = (ratingsCache.gMnfight >= 0)   ? globalReal(ctx, ratingsCache.gMnfight)   : 0.0;
        GMLReal myfight   = (ratingsCache.gMyfight >= 0)   ? globalReal(ctx, ratingsCache.gMyfight)   : 0.0;

        if (turntimer > 0.0 && mnfight == 2.0) {
            int32_t o_ob = selfInt(inst, ratingsCache.o_ob);
            o_ob = (o_ob == 0) ? 1 : 0;
            Instance_setSelfVar(inst, ratingsCache.o_ob, RValue_makeReal((GMLReal)o_ob));

            if (o_ob == 0) ratings += 1.0;
            if (o_ob == 1) ratings += 2.0;

            Instance* mettx = findInstanceByObject(runner, ratingsCache.objMettatonex);
            if (mettx != NULL && ratingsCache.mettxTurns >= 0) {
                GMLReal mtxTurns = selfReal(mettx, ratingsCache.mettxTurns);
                if (mtxTurns >= 20.0) ratings += 2.0;
            }
            globalSet(ctx, ratingsCache.gRatings, RValue_makeReal(ratings));
        }

        if (myfight == 0.0 && mnfight == 0.0) boastmode = 0;
        if (findInstanceByObject(runner, 410) != NULL) boastmode = 0;
    }
    Instance_setSelfVar(inst, ratingsCache.boastmode, RValue_makeReal((GMLReal)boastmode));

    if (heel == 1) {
        GMLReal mnfight = (ratingsCache.gMnfight >= 0) ? globalReal(ctx, ratingsCache.gMnfight) : 0.0;
        GMLReal myfight = (ratingsCache.gMyfight >= 0) ? globalReal(ctx, ratingsCache.gMyfight) : 0.0;
        if (myfight == 0.0 && mnfight == 0.0) {
            heel = 0;
            Instance_setSelfVar(inst, ratingsCache.heel, RValue_makeReal(0.0));
        }
    }

    
    Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);

    int32_t novel_armor = selfInt(inst, ratingsCache.novel_armor);
    if (novel_armor == 1) {
        Instance_setSelfVar(inst, ratingsCache.curtype, RValue_makeReal(6.0));
        Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0);
        Instance_setSelfVar(inst, ratingsCache.novel_armor, RValue_makeReal(0.0));
    }

    {
        GMLReal mnfight = (ratingsCache.gMnfight >= 0) ? globalReal(ctx, ratingsCache.gMnfight) : 0.0;
        GMLReal myfight = (ratingsCache.gMyfight >= 0) ? globalReal(ctx, ratingsCache.gMyfight) : 0.0;
        if (mnfight == 0.0 && myfight == 0.0) {
            GMLReal timeloss = selfReal(inst, ratingsCache.timeloss) + 1.0;
            int32_t o_o = selfInt(inst, ratingsCache.o_o) + 1;
            if (o_o > 3) o_o = 0;

            if (timeloss < 4000.0 && o_o == 0) {
                ratings -= 1.0;
                globalSet(ctx, ratingsCache.gRatings, RValue_makeReal(ratings));
            }
            Instance_setSelfVar(inst, ratingsCache.timeloss, RValue_makeReal(timeloss));
            Instance_setSelfVar(inst, ratingsCache.o_o, RValue_makeReal((GMLReal)o_o));
        }
    }
}





static struct {
    int32_t drawblack, bl, bly, x_maroon;
    bool ready;
} mettbossEventCache = { .ready = false };

static void initMettbossEventCache(DataWin* dw) {
    mettbossEventCache.drawblack = findSelfVarId(dw, "drawblack");
    mettbossEventCache.bl        = findSelfVarId(dw, "bl");
    mettbossEventCache.bly       = findSelfVarId(dw, "bly");
    mettbossEventCache.x_maroon  = findSelfVarId(dw, "x_maroon");
    mettbossEventCache.ready = (mettbossEventCache.drawblack >= 0 && mettbossEventCache.bl >= 0 &&
                                mettbossEventCache.bly >= 0 && mettbossEventCache.x_maroon >= 0);
}

static void native_mettbossEvent_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!mettbossEventCache.ready || runner->renderer == NULL) return;
    if (selfInt(inst, mettbossEventCache.drawblack) != 1) return;
    Renderer* r = runner->renderer;

    GMLReal bl = selfReal(inst, mettbossEventCache.bl);
    if (bl < 20.0) bl += 4.0;
    if (bl > 20.0) {
        bl = 20.0;
        BuiltinFunc sp = VMBuiltins_find("snd_play");
        if (sp) {
            RValue arg = RValue_makeReal(107.0);
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue rr = sp(ctx, &arg, 1);
            ctx->currentInstance = saved;
            RValue_free(&rr);
        }
    }
    Instance_setSelfVar(inst, mettbossEventCache.bl, RValue_makeReal(bl));

    
    
    uint32_t gmlCol = (uint32_t)selfInt(inst, mettbossEventCache.x_maroon);
    uint32_t red   = gmlCol & 0xFFu;
    uint32_t green = (gmlCol >> 8) & 0xFFu;
    uint32_t blue  = (gmlCol >> 16) & 0xFFu;
    uint32_t bgrCol = (blue << 16) | (green << 8) | red;

    GMLReal bly = selfReal(inst, mettbossEventCache.bly);
    
    
    
    float b1x1 = 140.0f, b1x2 = 140.0f + (float)bl;
    float b1y1 = 840.0f + (float)bly, b1y2 = 880.0f;
    if (b1y1 > b1y2) { float t = b1y1; b1y1 = b1y2; b1y2 = t; }
    r->vtable->drawRectangle(r, b1x1, b1y1, b1x2, b1y2, bgrCol, 1.0f, false);

    
    float b2x1 = 180.0f - (float)bl, b2x2 = 180.0f;
    float b2y1 = 840.0f + (float)bly, b2y2 = 880.0f;
    if (b2y1 > b2y2) { float t = b2y1; b2y1 = b2y2; b2y2 = t; }
    r->vtable->drawRectangle(r, b2x1, b2y1, b2x2, b2y2, bgrCol, 1.0f, false);
}




static struct {
    int32_t xbefore, xafter, ybefore, yafter, anim, g;
    bool ready;
} plusbombExplCache = { .ready = false };

static void initPlusbombExplCache(DataWin* dw) {
    plusbombExplCache.xbefore = findSelfVarId(dw, "xbefore");
    plusbombExplCache.xafter  = findSelfVarId(dw, "xafter");
    plusbombExplCache.ybefore = findSelfVarId(dw, "ybefore");
    plusbombExplCache.yafter  = findSelfVarId(dw, "yafter");
    plusbombExplCache.anim    = findSelfVarId(dw, "anim");
    plusbombExplCache.g       = findSelfVarId(dw, "g");
    plusbombExplCache.ready = (plusbombExplCache.anim >= 0);
}

static void native_plusbombExpl_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!plusbombExplCache.ready || runner->renderer == NULL || !runner->currentRoom) return;
    Renderer* r = runner->renderer;

    float roomW = (float)runner->currentRoom->width;
    float roomH = (float)runner->currentRoom->height;

    int32_t xbefore = (int32_t)ceilf(inst->x / 20.0f);
    int32_t xafter  = (int32_t)ceilf((roomW / 20.0f) - (inst->x / 20.0f));
    int32_t ybefore = (int32_t)ceilf(inst->y / 20.0f);
    int32_t yafter  = (int32_t)ceilf((roomH / 20.0f) - (inst->y / 20.0f));

    Instance_setSelfVar(inst, plusbombExplCache.xbefore, RValue_makeReal((GMLReal)xbefore));
    Instance_setSelfVar(inst, plusbombExplCache.xafter,  RValue_makeReal((GMLReal)xafter));
    Instance_setSelfVar(inst, plusbombExplCache.ybefore, RValue_makeReal((GMLReal)ybefore));
    Instance_setSelfVar(inst, plusbombExplCache.yafter,  RValue_makeReal((GMLReal)yafter));

    GMLReal anim = selfReal(inst, plusbombExplCache.anim);
    int32_t subimg = (int32_t)anim;

    for (int32_t i = 0; i < ybefore + 1; i++)
        Renderer_drawSprite(r, 540, subimg, inst->x, inst->y - 20.0f - (float)(i * 20));
    for (int32_t i = 0; i < yafter + 1; i++)
        Renderer_drawSprite(r, 540, subimg, inst->x, inst->y + 20.0f + (float)(i * 20));
    for (int32_t i = 0; i < xbefore + 1; i++)
        Renderer_drawSprite(r, 538, subimg, inst->x - 20.0f - (float)(i * 20), inst->y);
    for (int32_t i = 0; i < xafter + 1; i++)
        Renderer_drawSprite(r, 538, subimg, inst->x + 20.0f + (float)(i * 20), inst->y);
    Renderer_drawSprite(r, 539, subimg, inst->x, inst->y);

    anim += 1.0;
    Instance_setSelfVar(inst, plusbombExplCache.anim, RValue_makeReal(anim));

    if (anim > 1.0 && anim < 3.0) {
        BuiltinFunc cr = VMBuiltins_find("collision_rectangle");
        bool hit = false;
        if (cr) {
            RValue a1[7] = {
                RValue_makeReal(0.0), RValue_makeReal(inst->y + 4.0),
                RValue_makeReal(roomW), RValue_makeReal(inst->y + 16.0),
                RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
            };
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = cr(ctx, a1, 7);
            ctx->currentInstance = saved;
            if (RValue_toInt32(res) >= 0) hit = true;
            RValue_free(&res);

            if (!hit) {
                RValue a2[7] = {
                    RValue_makeReal(inst->x + 4.0), RValue_makeReal(0.0),
                    RValue_makeReal(inst->x + 16.0), RValue_makeReal(roomH),
                    RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
                };
                saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                res = cr(ctx, a2, 7);
                ctx->currentInstance = saved;
                if (RValue_toInt32(res) >= 0) hit = true;
                RValue_free(&res);
            }
        }
        if (plusbombExplCache.g >= 0)
            Instance_setSelfVar(inst, plusbombExplCache.g, RValue_makeReal(hit ? 1.0 : 0.0));
        if (hit) Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
    }

    if (anim >= 7.0) Runner_destroyInstance(runner, inst);
}









static struct {
    int32_t siner, flash, frozen, blend2, op;
    int32_t magicfactor, magicfactor2, magicfactor3;  
    int32_t offx, offy, offs, offx2, offy2, offs2;
    int32_t gSoulRescue, gDebug;
    bool ready;
} floweyPipeCache = { .ready = false };

static void initFloweyPipeCache(VMContext* ctx, DataWin* dw) {
    floweyPipeCache.siner   = findSelfVarId(dw, "siner");
    floweyPipeCache.flash   = findSelfVarId(dw, "flash");
    floweyPipeCache.frozen  = findSelfVarId(dw, "frozen");
    floweyPipeCache.blend2  = findSelfVarId(dw, "blend2");
    floweyPipeCache.op      = findSelfVarId(dw, "op");
    floweyPipeCache.magicfactor  = findSelfVarId(dw, "magicfactor");
    floweyPipeCache.magicfactor2 = findSelfVarId(dw, "magicfactor2");
    floweyPipeCache.magicfactor3 = findSelfVarId(dw, "magicfactor3");
    floweyPipeCache.offx  = findSelfVarId(dw, "offx");
    floweyPipeCache.offy  = findSelfVarId(dw, "offy");
    floweyPipeCache.offs  = findSelfVarId(dw, "offs");
    floweyPipeCache.offx2 = findSelfVarId(dw, "offx2");
    floweyPipeCache.offy2 = findSelfVarId(dw, "offy2");
    floweyPipeCache.offs2 = findSelfVarId(dw, "offs2");
    floweyPipeCache.gSoulRescue = findGlobalVarId(ctx, "soul_rescue");
    floweyPipeCache.gDebug      = findGlobalVarId(ctx, "debug");
    floweyPipeCache.ready = (floweyPipeCache.siner >= 0 && floweyPipeCache.flash >= 0 &&
                             floweyPipeCache.frozen >= 0 && floweyPipeCache.blend2 >= 0 &&
                             floweyPipeCache.op >= 0 && floweyPipeCache.gSoulRescue >= 0);
}



static void floweyPipeProlog(VMContext* ctx, Runner* runner, Instance* inst,
                             GMLReal* outSiner, uint32_t* outBlend1, uint32_t* outBlend2,
                             int32_t variant ) {
    (void)ctx;
    int32_t frozen = selfInt(inst, floweyPipeCache.frozen);
    GMLReal siner = selfReal(inst, floweyPipeCache.siner);
    if (frozen == 0) siner += 1.0;
    Instance_setSelfVar(inst, floweyPipeCache.siner, RValue_makeReal(siner));
    *outSiner = siner;

    int32_t flash = selfInt(inst, floweyPipeCache.flash);
    uint32_t blend1 = inst->imageBlend;
    uint32_t blend2 = (uint32_t)selfInt(inst, floweyPipeCache.blend2);

    
    
    Renderer* r = runner ? runner->renderer : NULL;
    GMLReal op = selfReal(inst, floweyPipeCache.op);
    float alphaPulse = (float)(GMLReal_sin(siner / 3.0) / 2.0);
    if (alphaPulse < 0.0f) alphaPulse = 0.0f;
    if (alphaPulse > 1.0f) alphaPulse = 1.0f;

    if (flash == 0) { blend1 = 0xFFFFFFu; blend2 = 0xFFFFFFu; }
    else if (flash == 1) {
        
        
        
        
        if (r && r->vtable->drawEllipse) {
            float bx1, by1, bx2, by2;
            uint32_t ecol;
            if (variant == 1) {
                bx1 = inst->x - 70; by1 = inst->y + 50;
                bx2 = inst->x + 30; by2 = inst->y - 40;
                ecol = 0x00FFFFu;
            } else if (variant == 2) {
                bx1 = inst->x - 85; by1 = inst->y + 40;
                bx2 = inst->x + 85; by2 = inst->y - 40;
                ecol = 0xFF0000u;
            } else {
                bx1 = inst->x - 45; by1 = inst->y + 40;
                bx2 = inst->x + 45; by2 = inst->y - 40;
                ecol = 0x008000u;
            }
            if (bx1 > bx2) { float t = bx1; bx1 = bx2; bx2 = t; }
            if (by1 > by2) { float t = by1; by1 = by2; by2 = t; }
            float cx = (bx1 + bx2) * 0.5f, cy = (by1 + by2) * 0.5f;
            float rx = (bx2 - bx1) * 0.5f, ry = (by2 - by1) * 0.5f;
            r->vtable->drawEllipse(r, cx, cy, rx, ry, ecol, alphaPulse, false, r->circlePrecision);
        }

        
        if (variant == 1) {
            
            int32_t rr = (int32_t)(100.0 - GMLReal_sin(siner/3.0) * 100.0);
            if (rr < 0) rr = 0; if (rr > 255) rr = 255;
            blend1 = (uint32_t)rr | (230u << 8) | (255u << 16);
        } else if (variant == 2) {
            
            int32_t v = (int32_t)(120.0 - (GMLReal_sin(siner/3.0) / 2.0) * 100.0);
            if (v < 0) v = 0; if (v > 255) v = 255;
            blend1 = (uint32_t)v | ((uint32_t)v << 8) | (255u << 16);
        } else {
            
            int32_t v = (int32_t)(100.0 - GMLReal_sin(siner/3.0) * 100.0);
            if (v < 0) v = 0; if (v > 255) v = 255;
            blend1 = (uint32_t)v | (255u << 8) | ((uint32_t)v << 16);
        }
    }
    else if (flash == 2) {
        
        
        
        
        if (r && r->vtable->drawEllipse) {
            float bx1, by1, bx2, by2;
            uint32_t ecol;
            if (variant == 1) {
                bx1 = inst->x + (float)op + 70; by1 = inst->y + 50;
                bx2 = inst->x + (float)op - 30; by2 = inst->y - 40;
                ecol = 0x40A0FFu;
            } else if (variant == 2) {
                bx1 = inst->x - 105 + (float)op; by1 = inst->y + 40;
                bx2 = inst->x + 85 + (float)op;  by2 = inst->y - 40;
                ecol = 0xFF00FFu;
            } else {
                bx1 = inst->x - 45 + (float)op; by1 = inst->y + 40;
                bx2 = inst->x + 45 + (float)op; by2 = inst->y - 40;
                ecol = 0x00FFFFu;
            }
            if (bx1 > bx2) { float t = bx1; bx1 = bx2; bx2 = t; }
            if (by1 > by2) { float t = by1; by1 = by2; by2 = t; }
            float cx = (bx1 + bx2) * 0.5f, cy = (by1 + by2) * 0.5f;
            float rx = (bx2 - bx1) * 0.5f, ry = (by2 - by1) * 0.5f;
            r->vtable->drawEllipse(r, cx, cy, rx, ry, ecol, alphaPulse, false, r->circlePrecision);
        }

        if (variant == 1) {
            
            int32_t bb = (int32_t)(100.0 - GMLReal_sin(siner/3.0) * 100.0);
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            blend2 = 230u | (180u << 8) | ((uint32_t)bb << 16);
        } else if (variant == 2) {
            
            int32_t gg = (int32_t)(100.0 - GMLReal_sin(siner/3.0) * 100.0);
            if (gg < 0) gg = 0; if (gg > 255) gg = 255;
            blend2 = 230u | ((uint32_t)gg << 8) | (200u << 16);
        } else {
            
            int32_t bb = (int32_t)(130.0 - GMLReal_sin(siner/3.0) * 120.0);
            if (bb < 0) bb = 0; if (bb > 255) bb = 255;
            blend2 = 230u | (230u << 8) | ((uint32_t)bb << 16);
        }
    }

    inst->imageBlend = blend1;
    Instance_setSelfVar(inst, floweyPipeCache.blend2, RValue_makeReal((GMLReal)blend2));
    *outBlend1 = blend1;
    *outBlend2 = blend2;
}




static void native_floweyPipe_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyPipeCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    GMLReal siner; uint32_t blend1, blend2;
    floweyPipeProlog(ctx, runner, inst, &siner, &blend1, &blend2, 1);

    GMLReal soulRescue = globalReal(ctx, floweyPipeCache.gSoulRescue);
    GMLReal op = selfReal(inst, floweyPipeCache.op);
    int32_t subimg = (int32_t)inst->imageIndex;
    int32_t sprite = inst->spriteIndex;

    
    for (int32_t i = 0; i < 10; i++) {
        float offx = (float)(inst->x + GMLReal_sin((GMLReal)(i - 45) / 3.0) * 60.0) - (float)(i * 2);
        float offy = inst->y + (float)(GMLReal_cos((GMLReal)(i - 45) / 3.0) * 50.0
                                       + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 8.0);
        GMLReal offsRaw = GMLReal_sin((siner + (GMLReal)(i * 4)) / 6.0) * 2.0;
        float offs = (offsRaw > 1.0) ? (float)((offsRaw - 1.0) / 3.0) : 0.0f;
        float rot = (float)(i * 20 + 30);

        if (soulRescue < 1.0) {
            Renderer_drawSpriteExt(r, sprite, subimg, offx, offy,
                                   1.0f + offs, 1.0f + offs, rot, blend1, 1.0f);
        } else {
            offy = inst->y + (float)(GMLReal_cos((GMLReal)(i - 45) / 3.0) * 50.0
                                     + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 4.0);
            Renderer_drawSpriteExt(r, sprite, subimg, offx, offy,
                                   1.0f, 1.0f, rot, 0x808080u, 1.0f);  
        }
    }

    
    for (int32_t i = 0; i < 10; i++) {
        float offx2 = (float)(inst->x - GMLReal_sin((GMLReal)(i - 45) / 3.0) * 60.0)
                      + (float)(i * 2) + (float)op;
        float offy2 = inst->y + (float)(GMLReal_cos((GMLReal)(i - 45) / 3.0) * 50.0
                                        + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 8.0);
        GMLReal offsRaw = GMLReal_sin((siner + (GMLReal)(i * 4)) / 6.0) * 2.0;
        float offs2 = (offsRaw > 1.0) ? (float)((offsRaw - 1.0) / 3.0) : 0.0f;
        float rot = (float)(i * -20 + 150);

        if (soulRescue < 2.0) {
            Renderer_drawSpriteExt(r, sprite, subimg, offx2, offy2,
                                   1.0f + offs2, 1.0f + offs2, rot, blend2, 1.0f);
        } else {
            offy2 = inst->y + (float)(GMLReal_cos((GMLReal)(i - 45) / 3.0) * 50.0
                                      + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 4.0);
            Renderer_drawSpriteExt(r, sprite, subimg, offx2, offy2,
                                   1.0f, 1.0f, rot, 0x808080u, 1.0f);
        }
    }
}




static void native_floweyPipe2_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyPipeCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    GMLReal siner; uint32_t blend1, blend2;
    floweyPipeProlog(ctx, runner, inst, &siner, &blend1, &blend2, 2);

    GMLReal soulRescue = globalReal(ctx, floweyPipeCache.gSoulRescue);
    GMLReal op = selfReal(inst, floweyPipeCache.op);
    GMLReal magicfactor  = (floweyPipeCache.magicfactor  >= 0) ? selfReal(inst, floweyPipeCache.magicfactor)  : 0.0;
    GMLReal magicfactor2 = (floweyPipeCache.magicfactor2 >= 0) ? selfReal(inst, floweyPipeCache.magicfactor2) : 0.0;
    GMLReal magicfactor3 = (floweyPipeCache.magicfactor3 >= 0) ? selfReal(inst, floweyPipeCache.magicfactor3) : 0.0;
    int32_t subimg = (int32_t)inst->imageIndex;
    int32_t sprite = inst->spriteIndex;

    
    for (int32_t i = 0; i < 13; i++) {
        float offx = inst->x + (float)(GMLReal_sin((GMLReal)(i - 49) / 3.0) * 85.0);
        float offy = inst->y + (float)(GMLReal_cos((GMLReal)(i - 3) / 2.2) * 40.0
                                       + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 8.0);
        GMLReal offsRaw = GMLReal_sin((siner + (GMLReal)(i * 4)) / 6.0) * 2.0;
        float offs = (offsRaw > 1.0) ? (float)((offsRaw - 1.0) / 3.0) : 0.0f;

        float offv = (float)(i * -12 - 330);
        if (i > 5) offv = (float)(i * -20 - 310);
        if (i > 7) offv = (float)(i * -22 - 310);

        if (soulRescue < 3.0) {
            Renderer_drawSpriteExt(r, sprite, subimg, offx, offy,
                                   1.0f + offs,
                                   (1.0f + offs) - (float)(GMLReal_sin((GMLReal)i / 5.0) * 0.3),
                                   offv, blend1, 1.0f);
        } else {
            Renderer_drawSpriteExt(r, sprite, subimg, offx, offy,
                                   1.0f, 1.0f, offv, 0x808080u, 1.0f);
        }
    }

    
    for (int32_t i = 0; i < 13; i++) {
        float offx2 = inst->x - (float)(GMLReal_sin((GMLReal)(i - 49) / 3.0) * 85.0) + (float)op;
        float offy2 = inst->y + (float)(GMLReal_cos((GMLReal)(i - 3) / 2.2) * 40.0
                                        + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 8.0);
        GMLReal offsRaw = GMLReal_sin((siner + (GMLReal)(i * 4)) / 6.0) * 2.0;
        float offs2 = (offsRaw > 1.0) ? (float)((offsRaw - 1.0) / 3.0) : 0.0f;

        float offv2 = (float)(i * 12) + (float)magicfactor;
        if (i > 5) offv2 = (float)(i * 30) + (float)magicfactor2;
        if (i > 7) offv2 = (float)(i * 20) + (float)magicfactor3;

        if (soulRescue < 4.0) {
            Renderer_drawSpriteExt(r, sprite, subimg, offx2, offy2,
                                   -1.0f - offs2,
                                   (1.0f + offs2) - (float)(GMLReal_sin((GMLReal)i / 5.0) * 0.3),
                                   offv2, blend2, 1.0f);
        } else {
            Renderer_drawSpriteExt(r, sprite, subimg, offx2, offy2,
                                   -1.0f, 1.0f, offv2, 0x808080u, 1.0f);
        }
    }
}




static void native_floweyPipe3_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyPipeCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    GMLReal siner; uint32_t blend1, blend2;
    floweyPipeProlog(ctx, runner, inst, &siner, &blend1, &blend2, 3);

    GMLReal soulRescue = globalReal(ctx, floweyPipeCache.gSoulRescue);
    GMLReal op = selfReal(inst, floweyPipeCache.op);
    int32_t subimg = (int32_t)inst->imageIndex;
    int32_t sprite = inst->spriteIndex;

    
    for (int32_t i = 0; i < 16; i++) {
        float offx = inst->x + (float)(GMLReal_sin((GMLReal)(i - 34) / 3.0) * 45.0);
        float offy = inst->y + (float)(GMLReal_cos((GMLReal)(i - 34) / 3.0) * 40.0
                                       + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 8.0);
        GMLReal offsRaw = GMLReal_sin((siner + (GMLReal)(i * 4)) / 6.0) * 2.0;
        float offs = (offsRaw > 1.0) ? (float)((offsRaw - 1.0) / 3.0) : 0.0f;
        float rot = (float)(i * 20 + 50);

        if (soulRescue < 5.0) {
            Renderer_drawSpriteExt(r, sprite, subimg, offx, offy,
                                   1.0f + offs, 1.0f + offs, rot, blend1, 1.0f);
        } else {
            Renderer_drawSpriteExt(r, sprite, subimg, offx, offy,
                                   1.0f, 1.0f, rot, 0x808080u, 1.0f);
        }
    }

    
    for (int32_t i = 0; i < 16; i++) {
        float offx2 = inst->x - (float)(GMLReal_sin((GMLReal)(i - 34) / 3.0) * 45.0) + (float)op;
        float offy2 = inst->y + (float)(GMLReal_cos((GMLReal)(i - 34) / 3.0) * 40.0
                                        + GMLReal_cos((siner + (GMLReal)(i * 4)) / 6.0) * 8.0);
        GMLReal offsRaw = GMLReal_sin((siner + (GMLReal)(i * 4)) / 6.0) * 2.0;
        float offs2 = (offsRaw > 1.0) ? (float)((offsRaw - 1.0) / 3.0) : 0.0f;
        float rot = (float)(i * -20 - 50);

        if (soulRescue < 6.0) {
            Renderer_drawSpriteExt(r, sprite, subimg, offx2, offy2,
                                   1.0f + offs2, 1.0f + offs2, rot, blend2, 1.0f);
        } else {
            Renderer_drawSpriteExt(r, sprite, subimg, offx2, offy2,
                                   1.0f, 1.0f, rot, 0x808080u, 1.0f);
        }
    }
}





static struct {
    int32_t siner, siner2;
    bool ready;
} floweyBgdrawCache = { .ready = false };

static void initFloweyBgdrawCache(DataWin* dw) {
    floweyBgdrawCache.siner  = findSelfVarId(dw, "siner");
    floweyBgdrawCache.siner2 = findSelfVarId(dw, "siner2");
    floweyBgdrawCache.ready = (floweyBgdrawCache.siner >= 0);
}

static void native_floweyBgdraw_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!floweyBgdrawCache.ready) return;
    GMLReal siner = selfReal(inst, floweyBgdrawCache.siner);
    GMLReal siner2 = 0.0;
    
    for (int32_t i = 0; i < 8; i++) {
        siner2 = siner + (GMLReal)i;
        runner->backgrounds[i].alpha = (float)(0.5 + GMLReal_sin(siner2 / 8.0) * 0.4);
        runner->backgrounds[i].x    += (float)(GMLReal_sin(siner2 / 8.0) * 1.0);
    }
    if (floweyBgdrawCache.siner2 >= 0)
        Instance_setSelfVar(inst, floweyBgdrawCache.siner2, RValue_makeReal(siner2));
    Instance_setSelfVar(inst, floweyBgdrawCache.siner, RValue_makeReal(siner + 1.0));
}






static struct {
    int32_t onoff, mode, con, cntr, siner, anim, op, desperate, frozen, laugh, laughtimer;
    int32_t rotbonus, xbonus, ybonus;
    int32_t gDebug;
    bool ready;
} floweyMouthCache = { .ready = false };

static void initFloweyMouthCache(VMContext* ctx, DataWin* dw) {
    floweyMouthCache.onoff      = findSelfVarId(dw, "onoff");
    floweyMouthCache.mode       = findSelfVarId(dw, "mode");
    floweyMouthCache.con        = findSelfVarId(dw, "con");
    floweyMouthCache.cntr       = findSelfVarId(dw, "cntr");
    floweyMouthCache.siner      = findSelfVarId(dw, "siner");
    floweyMouthCache.anim       = findSelfVarId(dw, "anim");
    floweyMouthCache.op         = findSelfVarId(dw, "op");
    floweyMouthCache.desperate  = findSelfVarId(dw, "desperate");
    floweyMouthCache.frozen     = findSelfVarId(dw, "frozen");
    floweyMouthCache.laugh      = findSelfVarId(dw, "laugh");
    floweyMouthCache.laughtimer = findSelfVarId(dw, "laughtimer");
    floweyMouthCache.rotbonus   = findSelfVarId(dw, "rotbonus");
    floweyMouthCache.xbonus     = findSelfVarId(dw, "xbonus");
    floweyMouthCache.ybonus     = findSelfVarId(dw, "ybonus");
    floweyMouthCache.gDebug     = findGlobalVarId(ctx, "debug");
    floweyMouthCache.ready = (floweyMouthCache.onoff >= 0 && floweyMouthCache.mode >= 0 &&
                              floweyMouthCache.con >= 0 && floweyMouthCache.siner >= 0);
}

static void native_floweyMouth_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyMouthCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t onoff = selfInt(inst, floweyMouthCache.onoff) + 1;
    if (onoff > 3) onoff = 0;
    Instance_setSelfVar(inst, floweyMouthCache.onoff, RValue_makeReal((GMLReal)onoff));

    int32_t mode = selfInt(inst, floweyMouthCache.mode);
    int32_t con  = selfInt(inst, floweyMouthCache.con);
    GMLReal cntr = selfReal(inst, floweyMouthCache.cntr);
    GMLReal siner = selfReal(inst, floweyMouthCache.siner);
    GMLReal anim  = selfReal(inst, floweyMouthCache.anim);
    int32_t desperate = selfInt(inst, floweyMouthCache.desperate);
    int32_t frozen    = selfInt(inst, floweyMouthCache.frozen);
    int32_t laugh     = selfInt(inst, floweyMouthCache.laugh);
    int32_t laughtimer = selfInt(inst, floweyMouthCache.laughtimer);
    GMLReal rotbonus  = selfReal(inst, floweyMouthCache.rotbonus);
    GMLReal xbonus    = selfReal(inst, floweyMouthCache.xbonus);
    GMLReal ybonus    = selfReal(inst, floweyMouthCache.ybonus);

    
    if (findInstanceByObject(runner, 1645) == NULL) {
        if (mode == 0)
            r->vtable->drawRectangle(r, inst->x, inst->y + 8.0f, inst->x + 60.0f, inst->y + 110.0f,
                                     0x000000u, 1.0f, false);
        else if (mode == 1)
            r->vtable->drawRectangle(r, inst->x - 10.0f, inst->y + 8.0f, inst->x + 70.0f, inst->y + 110.0f,
                                     0x000000u, 1.0f, false);
        else if (mode == 2)
            r->vtable->drawRectangle(r, inst->x - 18.0f, inst->y + 8.0f, inst->x + 78.0f, inst->y + 110.0f,
                                     0x000000u, 1.0f, false);
    }

    
    if (con == 3) { con = 4; cntr = 0.0; inst->alarm[4] = 40; }

    
    if (con == 4) {
        cntr += 1.0;
        float spriteAlpha = (float)((cntr - (GMLReal)(onoff * 5)) / 15.0);
        if (spriteAlpha < 0.0f) spriteAlpha = 0.0f;
        if (spriteAlpha > 1.0f) spriteAlpha = 1.0f;
        Renderer_drawSpriteExt(r, 2350, 0, 274.0f, 202.0f - (float)(GMLReal_sin(siner / 4.0) * 3.0),
                               1.0f, 1.0f, 0.0f, 0xFFFFFFu, spriteAlpha);
    }

    
    if (con == 5) {
        laugh = 1;
        mode = 2;
        Instance* b = Runner_createInstance(runner, 271.0, 214.0, 1645);
        if (b) b->depth = inst->depth + 1;
        con = 6;
        inst->alarm[4] = 25;
    }

    
    if (con == 7) { laugh = 0; mode = 0; con = 0; }

    
    if (frozen == 0) {
        siner += 1.0;
        if (desperate == 1) siner += 0.4;
        anim += 0.25;
    }

    
    Renderer_drawSpriteExt(r, 2283, (int32_t)inst->imageIndex,
                           inst->x + 10.0f, inst->y, 1.0f, 1.0f, 0.0f,
                           inst->imageBlend, inst->imageAlpha);

    
    if (mode == 0) {
        if (rotbonus > 0.0) rotbonus -= 5.0; else rotbonus = 0.0;
        if (xbonus > 0.0) xbonus -= 2.0; else xbonus = 0.0;
        if (ybonus < 0.0) ybonus += 2.0; else ybonus = 0.0;
        ybonus = 0.0;  
    }
    if (mode == 1) {
        if (ybonus > -4.0) ybonus -= 2.0;
        if (xbonus < 6.0) xbonus += 2.0;
        if (rotbonus < 15.0) rotbonus += 5.0;
    }
    if (mode == 2) {
        if (ybonus > -8.0) ybonus -= 4.0;
        if (xbonus < 6.0) xbonus += 2.0;
        if (rotbonus < 24.0) rotbonus += 8.0;
    }

    float op = 60.0f;  
    Instance_setSelfVar(inst, floweyMouthCache.op, RValue_makeReal(60.0));

    
    float sinS2 = (float)GMLReal_sin(siner / 2.0);
    float cosS2 = (float)GMLReal_cos(siner / 2.0);
    float sinS4 = (float)GMLReal_sin(siner / 4.0);

    if (desperate == 0) {
        
        Renderer_drawSpriteExt(r, 2280, (int32_t)inst->imageIndex,
                               ((inst->x + sinS2 * 3.0f) - 20.0f) + (float)xbonus,
                               ((inst->y + cosS2) - 5.0f) + (float)ybonus,
                               1.0f, 1.0f + sinS4 * 0.03f, 0.0f - cosS2 - (float)rotbonus,
                               inst->imageBlend, 1.0f);
        Renderer_drawSpriteExt(r, 2281, (int32_t)inst->imageIndex,
                               inst->x + sinS2 * 3.0f + (float)xbonus,
                               inst->y + cosS2 + (float)ybonus,
                               1.0f, 1.0f + sinS4 * 0.03f, 0.0f - cosS2 - (float)rotbonus,
                               inst->imageBlend, 1.0f);
        
        Renderer_drawSpriteExt(r, 2280, (int32_t)inst->imageIndex,
                               ((inst->x - sinS2 * 3.0f) + op + 20.0f) - (float)xbonus,
                               ((inst->y + cosS2) - 5.0f) + (float)ybonus,
                               -1.0f, 1.0f + sinS4 * 0.03f, 0.0f + cosS2 + (float)rotbonus,
                               inst->imageBlend, 1.0f);
        Renderer_drawSpriteExt(r, 2281, (int32_t)inst->imageIndex,
                               (inst->x + op) - sinS2 * 3.0f - (float)xbonus,
                               inst->y + cosS2 + (float)ybonus,
                               -1.0f, 1.0f + sinS4 * 0.03f, 0.0f + cosS2 + (float)rotbonus,
                               inst->imageBlend, 1.0f);
    } else { 
        Renderer_drawSpriteExt(r, 2280, (int32_t)inst->imageIndex,
                               ((inst->x + sinS2 * 4.0f) - 20.0f) + (float)xbonus,
                               ((inst->y + cosS2) - 5.0f) + (float)ybonus,
                               1.0f, 1.0f + sinS4 * 0.06f, 0.0f - cosS2 - (float)rotbonus,
                               inst->imageBlend, 1.0f);
        Renderer_drawSpriteExt(r, 2281, (int32_t)inst->imageIndex,
                               inst->x + sinS2 * 4.0f + (float)xbonus,
                               inst->y + cosS2 + (float)ybonus,
                               1.0f, 1.0f + sinS4 * 0.05f, 0.0f - cosS2 - (float)rotbonus,
                               inst->imageBlend, 1.0f);
        Renderer_drawSpriteExt(r, 2280, (int32_t)inst->imageIndex,
                               ((inst->x - sinS2 * 4.0f) + op + 20.0f) - (float)xbonus,
                               ((inst->y + cosS2) - 5.0f) + (float)ybonus,
                               -1.0f, 1.0f + sinS4 * 0.06f, 0.0f + cosS2 + (float)rotbonus,
                               inst->imageBlend, 1.0f);
        Renderer_drawSpriteExt(r, 2281, (int32_t)inst->imageIndex,
                               (inst->x + op) - sinS2 * 4.0f - (float)xbonus,
                               inst->y + cosS2 + (float)ybonus,
                               -1.0f, 1.0f + sinS4 * 0.05f, 0.0f - cosS2 - (float)rotbonus,
                               inst->imageBlend, 1.0f);
    }

    
    Renderer_drawSpriteExt(r, 2282, (int32_t)anim,
                           ((inst->x + sinS2 * 3.0f) - 5.0f) + (float)xbonus,
                           inst->y - 10.0f, 1.0f, 1.0f + sinS4 * 0.03f, 0.0f + cosS2,
                           inst->imageBlend, 1.0f);
    Renderer_drawSpriteExt(r, 2282, (int32_t)anim,
                           (((inst->x + op) - sinS2 * 3.0f) + 5.0f) - (float)xbonus,
                           inst->y - 10.0f, -1.0f, 1.0f + sinS4 * 0.03f, 0.0f + cosS2,
                           inst->imageBlend, 1.0f);

    

    
    if (laugh == 1) {
        siner = 0.0;
        laughtimer += 1;
        switch (laughtimer) {
            case 1: rotbonus = -3; xbonus = -1; ybonus = 0; break;
            case 2: rotbonus = 6;  xbonus = 2;  ybonus = -1; break;
            case 3: rotbonus = 18; xbonus = 4;  ybonus = -3; break;
            case 4: rotbonus = 20; xbonus = 6;  ybonus = -4; break;
            case 5: rotbonus = 12; xbonus = 4;  ybonus = -3; break;
            case 6: rotbonus = 6;  xbonus = 2;  ybonus = -2; break;
            case 7: rotbonus = 0;  xbonus = 0;  ybonus = 0; break;
        }
        
        
        if (laughtimer == 6) laughtimer = 1;
    }

    
    Instance_setSelfVar(inst, floweyMouthCache.mode, RValue_makeReal((GMLReal)mode));
    Instance_setSelfVar(inst, floweyMouthCache.con,  RValue_makeReal((GMLReal)con));
    Instance_setSelfVar(inst, floweyMouthCache.cntr, RValue_makeReal(cntr));
    Instance_setSelfVar(inst, floweyMouthCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, floweyMouthCache.anim,  RValue_makeReal(anim));
    Instance_setSelfVar(inst, floweyMouthCache.laugh, RValue_makeReal((GMLReal)laugh));
    Instance_setSelfVar(inst, floweyMouthCache.laughtimer, RValue_makeReal((GMLReal)laughtimer));
    Instance_setSelfVar(inst, floweyMouthCache.rotbonus, RValue_makeReal(rotbonus));
    Instance_setSelfVar(inst, floweyMouthCache.xbonus, RValue_makeReal(xbonus));
    Instance_setSelfVar(inst, floweyMouthCache.ybonus, RValue_makeReal(ybonus));
}






static struct {
    int32_t con, frozen, siner, siner2, md, gr, grgr, grgrgr, grgrgrgr;
    int32_t opx, op, durara, oner, memorymode, wimpy, desperate;
    int32_t objVsfloweyHeart;
    bool ready;
} floweyEyeCache = { .ready = false };



static inline uint32_t floweyEyeColor(GMLReal siner2, int32_t r_off, int32_t g_off, int32_t b_off) {
    int32_t rv = (int32_t)(170.0 + GMLReal_sin((siner2 + (GMLReal)r_off) / 2.0) * 70.0);
    int32_t gv = (int32_t)(170.0 + GMLReal_sin((siner2 + (GMLReal)g_off) / 2.0) * 70.0);
    int32_t bv = (int32_t)(170.0 + GMLReal_sin((siner2 + (GMLReal)b_off) / 2.0) * 70.0);
    if (rv < 0) rv = 0; if (rv > 255) rv = 255;
    if (gv < 0) gv = 0; if (gv > 255) gv = 255;
    if (bv < 0) bv = 0; if (bv > 255) bv = 255;
    return (uint32_t)rv | ((uint32_t)gv << 8) | ((uint32_t)bv << 16);
}

static void initFloweyEyeCache(DataWin* dw) {
    floweyEyeCache.con      = findSelfVarId(dw, "con");
    floweyEyeCache.frozen   = findSelfVarId(dw, "frozen");
    floweyEyeCache.siner    = findSelfVarId(dw, "siner");
    floweyEyeCache.siner2   = findSelfVarId(dw, "siner2");
    floweyEyeCache.md       = findSelfVarId(dw, "md");
    floweyEyeCache.gr       = findSelfVarId(dw, "gr");
    floweyEyeCache.grgr     = findSelfVarId(dw, "grgr");
    floweyEyeCache.grgrgr   = findSelfVarId(dw, "grgrgr");
    floweyEyeCache.grgrgrgr = findSelfVarId(dw, "grgrgrgr");
    floweyEyeCache.opx      = findSelfVarId(dw, "opx");
    floweyEyeCache.op       = findSelfVarId(dw, "op");
    floweyEyeCache.durara   = findSelfVarId(dw, "durara");
    floweyEyeCache.oner     = findSelfVarId(dw, "oner");
    floweyEyeCache.memorymode = findSelfVarId(dw, "memorymode");
    floweyEyeCache.wimpy    = findSelfVarId(dw, "wimpy");
    floweyEyeCache.desperate = findSelfVarId(dw, "desperate");
    floweyEyeCache.objVsfloweyHeart = findObjectIndex(dw, "obj_vsflowey_heart");
    floweyEyeCache.ready = (floweyEyeCache.con >= 0 && floweyEyeCache.siner >= 0 &&
                            floweyEyeCache.md >= 0 && floweyEyeCache.gr >= 0);
}



static void floweyEyeSetBullet(VMContext* ctx, Runner* runner, Instance* bullet,
                               Instance* heart, GMLReal durara, int32_t wimpy, int32_t oner,
                               float sign) {
    if (!bullet || !heart) return;
    float speed = (wimpy == 0) ? (7.0f - (float)durara * 0.1f) : (3.0f - (float)durara * 0.1f);
    float frict = (wimpy == 0) ? (-0.2f + (float)durara * 0.012f) : (-0.1f - (float)durara * 0.02f);
    int32_t extraDir = (wimpy == 0) ? 9 : 14;

    BuiltinFunc mtp = VMBuiltins_find("move_towards_point");
    if (mtp) {
        RValue args[3] = {
            RValue_makeReal(heart->x + 8.0), RValue_makeReal(heart->y + 8.0), RValue_makeReal((double)speed)
        };
        Instance* saved = (Instance*)ctx->currentInstance;
        ctx->currentInstance = bullet;
        RValue res = mtp(ctx, args, 3);
        ctx->currentInstance = saved;
        RValue_free(&res);
    }
    bullet->friction = frict;
    bullet->direction += sign * 18.0f * (float)durara;
    if (oner == 1) bullet->direction += (float)extraDir;
    Instance_computeComponentsFromSpeed(bullet);
    (void)runner;
}

static void native_floweyEye_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyEyeCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t con = selfInt(inst, floweyEyeCache.con);
    int32_t frozen = selfInt(inst, floweyEyeCache.frozen);
    GMLReal siner = selfReal(inst, floweyEyeCache.siner);
    GMLReal siner2 = selfReal(inst, floweyEyeCache.siner2);
    int32_t md = selfInt(inst, floweyEyeCache.md);
    GMLReal gr = selfReal(inst, floweyEyeCache.gr);
    GMLReal grgr = selfReal(inst, floweyEyeCache.grgr);
    GMLReal grgrgr = selfReal(inst, floweyEyeCache.grgrgr);
    GMLReal grgrgrgr = selfReal(inst, floweyEyeCache.grgrgrgr);
    GMLReal op = selfReal(inst, floweyEyeCache.op);
    GMLReal durara = selfReal(inst, floweyEyeCache.durara);
    int32_t oner = selfInt(inst, floweyEyeCache.oner);
    int32_t wimpy = selfInt(inst, floweyEyeCache.wimpy);
    int32_t desperate = selfInt(inst, floweyEyeCache.desperate);

    
    if (con == 0) {
        if (frozen == 0) siner += 1.0;
        else if (frozen != 4) {
            GMLReal r1 = (double)rand()/(double)RAND_MAX * 4.0;
            GMLReal r2 = (double)rand()/(double)RAND_MAX * 4.0;
            inst->y = (float)(inst->ystart + r1 - r2);
        }
    }

    if (con == 1) { con = 3; inst->alarm[4] = 4; }

    

    if (con == 3) {
        siner2 += 1.2;
        md = 1;
        gr     = (GMLReal)floweyEyeColor(siner2, 0, 3, 6);
        grgr   = (GMLReal)floweyEyeColor(siner2, 1, 4, 7);
        grgrgr = (GMLReal)floweyEyeColor(siner2, 2, 4, 8);
    }

    if (con == 4) {
        con = 5;
        durara = 0.0;
        oner = (rand() & 1) ? 1 : 0;
        inst->alarm[4] = 7;
        inst->imageBlend = 0xFFFFFFu;
    }

    if (con == 5) {
        Instance* heart = findInstanceByObject(runner,
                          findObjectIndex(ctx->dataWin, "obj_vsflowey_heart"));
        
        Instance* eba = Runner_createInstance(runner, inst->x, inst->y, 1642);
        if (eba) {
            Instance_setSelfVar(eba, floweyEyeCache.memorymode,
                                Instance_getSelfVar(inst, floweyEyeCache.memorymode));
            Instance_setSelfVar(eba, floweyEyeCache.durara, RValue_makeReal(durara));
            Instance_setSelfVar(eba, floweyEyeCache.oner, RValue_makeReal((GMLReal)oner));
            floweyEyeSetBullet(ctx, runner, eba, heart, durara, wimpy, oner, -1.0f);
        }
        
        Instance* ebb = Runner_createInstance(runner, inst->x + op, inst->y, 1642);
        if (ebb) {
            Instance_setSelfVar(ebb, floweyEyeCache.memorymode,
                                Instance_getSelfVar(inst, floweyEyeCache.memorymode));
            Instance_setSelfVar(ebb, floweyEyeCache.durara, RValue_makeReal(durara));
            Instance_setSelfVar(ebb, floweyEyeCache.oner, RValue_makeReal((GMLReal)oner));
            floweyEyeSetBullet(ctx, runner, ebb, heart, durara, wimpy, oner, +1.0f);
        }
        durara += 1.0;
        md = 1;
        if (gr == 16777215.0) { gr = 255.0; grgr = 0.0; grgrgr = 0.0; }
        else { gr = 16777215.0; grgr = 16777215.0; grgrgr = 16777215.0; }
    }

    if (con == 6) {
        gr = 16777215.0; grgr = 16777215.0; grgrgr = 16777215.0;
        md = 0; inst->imageBlend = 0xFFFFFFu; con = 0;
    }

    if (con == 10) {
        siner2 += 1.2;
        md = 1;
        gr     = (GMLReal)floweyEyeColor(siner2, 0, 3, 6);
        grgr   = (GMLReal)floweyEyeColor(siner2, 1, 4, 7);
        grgrgr = (GMLReal)floweyEyeColor(siner2, 2, 4, 8);
    }

    if (desperate == 1 && frozen == 0) siner += 0.5;

    
    float cosS3 = (float)GMLReal_cos(siner / 3.0);
    float sinS3 = (float)GMLReal_sin(siner / 3.0);
    float sinS4 = (float)GMLReal_sin(siner / 4.0);
    float sinS2 = (float)GMLReal_sin(siner / 2.0);
    int32_t imgIdx = (int32_t)inst->imageIndex;
    uint32_t blend = inst->imageBlend;

    
    Renderer_drawSpriteExt(r, 2272, imgIdx, inst->x, inst->y + cosS3 * 2.0f,
                           0.8f, 0.8f, sinS4 * 2.0f, (uint32_t)(int32_t)grgrgrgr, 1.0f);
    Renderer_drawSpriteExt(r, 2269, imgIdx, inst->x - 5.0f, inst->y + cosS3 * 3.0f,
                           1.0f, 1.0f, sinS4 * 2.0f, (uint32_t)(int32_t)grgrgrgr, 1.0f);

    
    if (md == 0) {
        Renderer_drawSpriteExt(r, 2274, imgIdx, inst->x, -4.0f + inst->y + sinS3 * 2.0f,
                               0.8f, 0.8f, sinS2 * 4.0f, blend, 1.0f);
        Renderer_drawSpriteExt(r, 2278, imgIdx, inst->x, -6.0f + inst->y + sinS3 * 4.0f,
                               0.8f, 0.8f, sinS2 * 4.0f, blend, 1.0f);
        if (desperate == 0) {
            Renderer_drawSpriteExt(r, 2276, imgIdx, inst->x, -5.0f + inst->y + sinS3 * 2.0f,
                                   0.8f - sinS3 * 0.4f, 1.0f - sinS3 * 0.4f, 0.0f, blend, 1.0f);
        } else {  
            float yo = -5.0f + inst->y + sinS3 * 2.5f;
            if (frozen == 0 || frozen == 4) {
                Renderer_drawSpriteExt(r, 2276, imgIdx, inst->x, yo,
                                       0.8f - sinS3 * 0.3f, 0.8f - sinS3 * 0.3f, 0.0f, blend, 1.0f);
            } else {
                Renderer_drawSpriteExt(r, 2276, imgIdx, inst->x, yo,
                                       0.7f - sinS3 * 0.1f, 0.7f - sinS3 * 0.1f, 0.0f, blend, 1.0f);
            }
        }
    }
    
    if (md == 1) {
        Renderer_drawSpriteExt(r, 2275, imgIdx, inst->x, -4.0f + inst->y + sinS3 * 2.0f,
                               0.8f, 0.8f, sinS2 * 4.0f, (uint32_t)(int32_t)grgrgr, 1.0f);
        Renderer_drawSpriteExt(r, 2279, imgIdx, inst->x, -6.0f + inst->y + sinS3 * 4.0f,
                               0.8f, 0.8f, sinS2 * 4.0f, (uint32_t)(int32_t)grgr, 1.0f);
        Renderer_drawSpriteExt(r, 2277, imgIdx, inst->x, -5.0f + inst->y + sinS3 * 2.0f,
                               0.8f - sinS3 * 0.4f, 1.0f - sinS3 * 0.4f, 0.0f,
                               (uint32_t)(int32_t)gr, 1.0f);
    }

    Renderer_drawSpriteExt(r, 2271, imgIdx, inst->x, inst->y + cosS3 * 2.0f,
                           0.8f, 0.8f, sinS4 * 2.0f, blend, 1.0f);

    
    op = 126.0;

    
    Renderer_drawSpriteExt(r, 2272, imgIdx, inst->x + (float)op, inst->y + cosS3 * 2.0f,
                           -0.8f, 0.8f, -sinS4 * 2.0f, (uint32_t)(int32_t)grgrgrgr, 1.0f);
    Renderer_drawSpriteExt(r, 2269, imgIdx, inst->x + 5.0f + (float)op, inst->y + cosS3 * 3.0f,
                           -1.0f, 1.0f, sinS4 * 2.0f, (uint32_t)(int32_t)grgrgrgr, 1.0f);

    if (md == 0) {
        Renderer_drawSpriteExt(r, 2274, imgIdx, inst->x + (float)op, -4.0f + inst->y + sinS3 * 2.0f,
                               -0.8f, 0.8f, -sinS2 * 4.0f, blend, 1.0f);
        Renderer_drawSpriteExt(r, 2278, imgIdx, inst->x + (float)op, -6.0f + inst->y + sinS3 * 4.0f,
                               -0.8f, 0.8f, -sinS2 * 4.0f, blend, 1.0f);
        if (desperate == 0) {
            Renderer_drawSpriteExt(r, 2276, imgIdx, inst->x + (float)op, -5.0f + inst->y + sinS3 * 2.0f,
                                   -1.0f + cosS3 * 0.4f, 1.0f - cosS3 * 0.4f, 0.0f, blend, 1.0f);
        } else {
            float yo = -5.0f + inst->y + sinS3 * 2.5f;
            if (frozen == 0 || frozen == 4) {
                Renderer_drawSpriteExt(r, 2276, imgIdx, inst->x + (float)op, yo,
                                       -0.9f + cosS3 * 0.3f, 0.9f - cosS3 * 0.3f, 0.0f, blend, 1.0f);
            } else {
                Renderer_drawSpriteExt(r, 2276, imgIdx, inst->x + (float)op, yo,
                                       -0.7f + cosS3 * 0.1f, 0.7f - cosS3 * 0.1f, 0.0f, blend, 1.0f);
            }
        }
    }
    if (md == 1) {
        Renderer_drawSpriteExt(r, 2275, imgIdx, inst->x + (float)op, -4.0f + inst->y + sinS3 * 2.0f,
                               -0.8f, 0.8f, -sinS2 * 4.0f, (uint32_t)(int32_t)grgrgr, 1.0f);
        Renderer_drawSpriteExt(r, 2279, imgIdx, inst->x + (float)op, -6.0f + inst->y + sinS3 * 4.0f,
                               -0.8f, 0.8f, -sinS2 * 4.0f, (uint32_t)(int32_t)grgr, 1.0f);
        Renderer_drawSpriteExt(r, 2277, imgIdx, inst->x + (float)op, -5.0f + inst->y + sinS3 * 2.0f,
                               -1.0f + cosS3 * 0.4f, 1.0f - cosS3 * 0.4f, 0.0f,
                               (uint32_t)(int32_t)gr, 1.0f);
    }

    Renderer_drawSpriteExt(r, 2271, imgIdx, inst->x + (float)op, inst->y + cosS3 * 2.0f,
                           -0.8f, 0.8f, -sinS4 * 2.0f, blend, 1.0f);

    
    Instance_setSelfVar(inst, floweyEyeCache.con,    RValue_makeReal((GMLReal)con));
    Instance_setSelfVar(inst, floweyEyeCache.siner,  RValue_makeReal(siner));
    Instance_setSelfVar(inst, floweyEyeCache.siner2, RValue_makeReal(siner2));
    Instance_setSelfVar(inst, floweyEyeCache.md,     RValue_makeReal((GMLReal)md));
    Instance_setSelfVar(inst, floweyEyeCache.gr,     RValue_makeReal(gr));
    Instance_setSelfVar(inst, floweyEyeCache.grgr,   RValue_makeReal(grgr));
    Instance_setSelfVar(inst, floweyEyeCache.grgrgr, RValue_makeReal(grgrgr));
    Instance_setSelfVar(inst, floweyEyeCache.op,     RValue_makeReal(op));
    Instance_setSelfVar(inst, floweyEyeCache.durara, RValue_makeReal(durara));
    Instance_setSelfVar(inst, floweyEyeCache.oner,   RValue_makeReal((GMLReal)oner));
}




static struct {
    int32_t siner, frozen;
    int32_t msin, ysin, growth, msin2, ysin2, growth2, msin3, ysin3, growth3, blend3;
    bool ready;
} bgpipeCache = { .ready = false };

static void initBgpipeCache(DataWin* dw) {
    bgpipeCache.siner   = findSelfVarId(dw, "siner");
    bgpipeCache.frozen  = findSelfVarId(dw, "frozen");
    bgpipeCache.msin    = findSelfVarId(dw, "msin");
    bgpipeCache.ysin    = findSelfVarId(dw, "ysin");
    bgpipeCache.growth  = findSelfVarId(dw, "growth");
    bgpipeCache.msin2   = findSelfVarId(dw, "msin2");
    bgpipeCache.ysin2   = findSelfVarId(dw, "ysin2");
    bgpipeCache.growth2 = findSelfVarId(dw, "growth2");
    bgpipeCache.msin3   = findSelfVarId(dw, "msin3");
    bgpipeCache.ysin3   = findSelfVarId(dw, "ysin3");
    bgpipeCache.growth3 = findSelfVarId(dw, "growth3");
    bgpipeCache.blend3  = findSelfVarId(dw, "blend3");
    bgpipeCache.ready = (bgpipeCache.siner >= 0 && bgpipeCache.frozen >= 0);
}


static inline uint32_t mergeColor(uint32_t c1, uint32_t c2, float amount) {
    int32_t r1 = (int32_t)(c1 & 0xFF), g1 = (int32_t)((c1 >> 8) & 0xFF), b1 = (int32_t)((c1 >> 16) & 0xFF);
    int32_t r2 = (int32_t)(c2 & 0xFF), g2 = (int32_t)((c2 >> 8) & 0xFF), b2 = (int32_t)((c2 >> 16) & 0xFF);
    float inv = 1.0f - amount;
    int32_t r = (int32_t)((float)r1 * inv + (float)r2 * amount);
    int32_t g = (int32_t)((float)g1 * inv + (float)g2 * amount);
    int32_t b = (int32_t)((float)b1 * inv + (float)b2 * amount);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

static void native_bgpipe_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!bgpipeCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t frozen = selfInt(inst, bgpipeCache.frozen);
    GMLReal siner = selfReal(inst, bgpipeCache.siner);
    if (frozen == 0) siner += 1.0;
    Instance_setSelfVar(inst, bgpipeCache.siner, RValue_makeReal(siner));

    GMLReal msin    = GMLReal_sin(siner / 9.0) * 2.0;
    GMLReal ysin    = GMLReal_cos(siner / 8.0) * 1.0;
    GMLReal growth  = GMLReal_sin(siner / 7.0) * 0.012;
    GMLReal msin2   = GMLReal_sin(siner / 10.0) * 1.5;
    GMLReal ysin2   = GMLReal_cos(siner / 9.0) * 0.8;
    GMLReal growth2 = GMLReal_sin(siner / 8.0) * 0.01;
    GMLReal msin3   = GMLReal_sin(siner / 11.0) * 1.0;
    GMLReal ysin3   = GMLReal_cos(siner / 10.0) * 0.5;
    GMLReal growth3 = GMLReal_sin(siner / 9.0) * 0.005;
    uint32_t blend3 = mergeColor(inst->imageBlend, 0x000000u, 0.4f);

    
    if (bgpipeCache.msin >= 0)    Instance_setSelfVar(inst, bgpipeCache.msin,    RValue_makeReal(msin));
    if (bgpipeCache.ysin >= 0)    Instance_setSelfVar(inst, bgpipeCache.ysin,    RValue_makeReal(ysin));
    if (bgpipeCache.growth >= 0)  Instance_setSelfVar(inst, bgpipeCache.growth,  RValue_makeReal(growth));
    if (bgpipeCache.msin2 >= 0)   Instance_setSelfVar(inst, bgpipeCache.msin2,   RValue_makeReal(msin2));
    if (bgpipeCache.ysin2 >= 0)   Instance_setSelfVar(inst, bgpipeCache.ysin2,   RValue_makeReal(ysin2));
    if (bgpipeCache.growth2 >= 0) Instance_setSelfVar(inst, bgpipeCache.growth2, RValue_makeReal(growth2));
    if (bgpipeCache.msin3 >= 0)   Instance_setSelfVar(inst, bgpipeCache.msin3,   RValue_makeReal(msin3));
    if (bgpipeCache.ysin3 >= 0)   Instance_setSelfVar(inst, bgpipeCache.ysin3,   RValue_makeReal(ysin3));
    if (bgpipeCache.growth3 >= 0) Instance_setSelfVar(inst, bgpipeCache.growth3, RValue_makeReal(growth3));
    if (bgpipeCache.blend3 >= 0)  Instance_setSelfVar(inst, bgpipeCache.blend3,  RValue_makeReal((GMLReal)blend3));

    int32_t subimg = (int32_t)inst->imageIndex;
    int32_t sprite = inst->spriteIndex;
    float ixs = inst->imageXscale;

    
    Renderer_drawSpriteExt(r, sprite, subimg,
                           inst->x + (float)((msin3 - 60.0) * (double)ixs),
                           inst->y + (float)ysin3 - 20.0f,
                           (float)growth3 + ixs, 1.0f, 0.0f, blend3, 1.0f);
    Renderer_drawSpriteExt(r, sprite, subimg,
                           inst->x + (float)((msin2 - 40.0) * (double)ixs),
                           inst->y + (float)ysin2 - 10.0f,
                           (float)growth2 + ixs, 1.0f, 0.0f, blend3, 1.0f);
    Renderer_drawSpriteExt(r, sprite, subimg,
                           inst->x + (float)(msin * (double)ixs),
                           inst->y + (float)ysin,
                           (float)growth + ixs, 1.0f, 0.0f, inst->imageBlend, 1.0f);
}




static struct {
    int32_t siner, frozen, growth, growth2, growth3, ssx, ssx2, ssx3;
    bool ready;
} vinesCache = { .ready = false };

static void initVinesCache(DataWin* dw) {
    vinesCache.siner   = findSelfVarId(dw, "siner");
    vinesCache.frozen  = findSelfVarId(dw, "frozen");
    vinesCache.growth  = findSelfVarId(dw, "growth");
    vinesCache.growth2 = findSelfVarId(dw, "growth2");
    vinesCache.growth3 = findSelfVarId(dw, "growth3");
    vinesCache.ssx     = findSelfVarId(dw, "ssx");
    vinesCache.ssx2    = findSelfVarId(dw, "ssx2");
    vinesCache.ssx3    = findSelfVarId(dw, "ssx3");
    vinesCache.ready = (vinesCache.siner >= 0 && vinesCache.frozen >= 0);
}

static void native_vinesFlowey_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!vinesCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t frozen = selfInt(inst, vinesCache.frozen);
    GMLReal siner = selfReal(inst, vinesCache.siner);
    if (frozen == 0) siner += 1.0;
    Instance_setSelfVar(inst, vinesCache.siner, RValue_makeReal(siner));

    float ixs = inst->imageXscale;
    float growth  = 1.0f + (float)(GMLReal_sin(siner / 6.0) * 0.05);
    float growth2 = 1.0f + (float)(GMLReal_cos(siner / 6.0) * 0.05);
    float growth3 = 1.0f - (float)(GMLReal_sin(siner / 7.0) * 0.05);
    float ssx  = (float)(GMLReal_sin(siner / 4.0) * 2.0) * ixs;
    float ssx2 = (float)(GMLReal_sin(siner / 5.0) * 1.0) * ixs;
    float ssx3 = (float)(GMLReal_cos(siner / 6.0) * 0.5) * ixs;

    if (vinesCache.growth >= 0)  Instance_setSelfVar(inst, vinesCache.growth,  RValue_makeReal(growth));
    if (vinesCache.growth2 >= 0) Instance_setSelfVar(inst, vinesCache.growth2, RValue_makeReal(growth2));
    if (vinesCache.growth3 >= 0) Instance_setSelfVar(inst, vinesCache.growth3, RValue_makeReal(growth3));
    if (vinesCache.ssx >= 0)   Instance_setSelfVar(inst, vinesCache.ssx,   RValue_makeReal(ssx));
    if (vinesCache.ssx2 >= 0)  Instance_setSelfVar(inst, vinesCache.ssx2,  RValue_makeReal(ssx2));
    if (vinesCache.ssx3 >= 0)  Instance_setSelfVar(inst, vinesCache.ssx3,  RValue_makeReal(ssx3));

    int32_t subimg = (int32_t)inst->imageIndex;
    uint32_t blend = inst->imageBlend;
    Renderer_drawSpriteExt(r, 2298, subimg, (inst->x - ssx3) + 20.0f, inst->y,
                           ixs, growth3, 0.0f, blend, 1.0f);
    Renderer_drawSpriteExt(r, 2297, subimg, inst->x - ssx2 - 20.0f, inst->y,
                           ixs, growth2, 0.0f, blend, 1.0f);
    Renderer_drawSpriteExt(r, 2296, subimg, inst->x - ssx, inst->y,
                           ixs, growth, 0.0f, blend, 1.0f);
}












static struct {
    int32_t rot, rotx, roty;   
    bool ready;
} floweyLeftEyeCache = { .ready = false };

static void initFloweyLeftEyeCache(DataWin* dw) {
    floweyLeftEyeCache.rot  = findSelfVarId(dw, "rot");
    floweyLeftEyeCache.rotx = findSelfVarId(dw, "rotx");
    floweyLeftEyeCache.roty = findSelfVarId(dw, "roty");
    floweyLeftEyeCache.ready = true; 
}




static void floweyLeftEyeSetBullet(VMContext* ctx, Instance* bullet, Instance* heart,
                                   GMLReal durara, int32_t wimpy) {
    if (!bullet || !heart) return;
    float speed = (wimpy == 0) ? 12.0f : 5.0f;
    float frict = (wimpy == 0) ? -0.2f : -0.1f;
    BuiltinFunc mtp = VMBuiltins_find("move_towards_point");
    if (mtp) {
        RValue args[3] = {
            RValue_makeReal(heart->x + 8.0), RValue_makeReal(heart->y + 8.0),
            RValue_makeReal((double)speed)
        };
        Instance* saved = (Instance*)ctx->currentInstance;
        ctx->currentInstance = bullet;
        RValue res = mtp(ctx, args, 3);
        ctx->currentInstance = saved;
        RValue_free(&res);
    }
    bullet->friction = frict;
    float dirBump = (wimpy == 0) ? (18.0f - (float)durara * 18.0f)
                                 : (30.0f - (float)durara * 30.0f);
    bullet->direction += dirBump;
    Instance_computeComponentsFromSpeed(bullet);
}

static void native_floweyLeftEye_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyEyeCache.ready || runner->renderer == NULL) return;  
    Renderer* r = runner->renderer;

    int32_t con = selfInt(inst, floweyEyeCache.con);
    int32_t frozen = selfInt(inst, floweyEyeCache.frozen);
    GMLReal siner = selfReal(inst, floweyEyeCache.siner);
    GMLReal siner2 = selfReal(inst, floweyEyeCache.siner2);
    int32_t md = selfInt(inst, floweyEyeCache.md);
    GMLReal gr = selfReal(inst, floweyEyeCache.gr);
    GMLReal grgr = selfReal(inst, floweyEyeCache.grgr);
    GMLReal grgrgr = selfReal(inst, floweyEyeCache.grgrgr);
    GMLReal op = selfReal(inst, floweyEyeCache.op);
    GMLReal durara = selfReal(inst, floweyEyeCache.durara);
    int32_t oner = selfInt(inst, floweyEyeCache.oner);
    int32_t wimpy = selfInt(inst, floweyEyeCache.wimpy);
    int32_t desperate = selfInt(inst, floweyEyeCache.desperate);

    
    if (con == 0) {
        if (frozen == 0) {
            siner += 1.0;
            if (desperate == 1) siner += 0.1;
        } else if (frozen != 4) {
            GMLReal r1 = (double)rand()/(double)RAND_MAX * 6.0;
            GMLReal r2 = (double)rand()/(double)RAND_MAX * 6.0;
            inst->x = (float)(inst->xstart + r1 - r2);
        }
    }

    if (con == 1) { con = 3; inst->alarm[4] = 2; }

    if (con == 3) {
        siner2 += 1.2;
        md = 1;
        gr     = (GMLReal)floweyEyeColor(siner2, 0, 3, 6);
        grgr   = (GMLReal)floweyEyeColor(siner2, 1, 4, 7);
        grgrgr = (GMLReal)floweyEyeColor(siner2, 2, 4, 8);
    }

    if (con == 4) {
        con = 5;
        durara = 0.0;
        oner = (rand() & 1) ? 1 : 0;
        inst->alarm[4] = 3;  
        inst->imageBlend = 0xFFFFFFu;
    }

    if (con == 5) {
        Instance* heart = findInstanceByObject(runner,
                          findObjectIndex(ctx->dataWin, "obj_vsflowey_heart"));
        Instance* eba = Runner_createInstance(runner, inst->x, inst->y, 1642);
        if (eba) {
            Instance_setSelfVar(eba, floweyEyeCache.memorymode,
                                Instance_getSelfVar(inst, floweyEyeCache.memorymode));
            Instance_setSelfVar(eba, floweyEyeCache.durara, RValue_makeReal(durara));
            Instance_setSelfVar(eba, floweyEyeCache.oner, RValue_makeReal((GMLReal)oner));
            floweyLeftEyeSetBullet(ctx, eba, heart, durara, wimpy);
        }
        Instance* ebb = Runner_createInstance(runner, inst->x + op, inst->y, 1642);
        if (ebb) {
            Instance_setSelfVar(ebb, floweyEyeCache.memorymode,
                                Instance_getSelfVar(inst, floweyEyeCache.memorymode));
            Instance_setSelfVar(ebb, floweyEyeCache.durara, RValue_makeReal(durara));
            Instance_setSelfVar(ebb, floweyEyeCache.oner, RValue_makeReal((GMLReal)oner));
            floweyLeftEyeSetBullet(ctx, ebb, heart, durara, wimpy);
        }
        durara += 1.0;
        md = 1;
        if (gr == 16777215.0) { gr = 255.0; grgr = 0.0; grgrgr = 0.0; }
        else { gr = 16777215.0; grgr = 16777215.0; grgrgr = 16777215.0; }
    }

    if (con == 6) {
        gr = 16777215.0; grgr = 16777215.0; grgrgr = 16777215.0;
        md = 0; inst->imageBlend = 0xFFFFFFu; con = 0;
    }

    if (con == 10) {
        siner2 += 1.2;
        md = 1;
        gr     = (GMLReal)floweyEyeColor(siner2, 0, 3, 6);
        grgr   = (GMLReal)floweyEyeColor(siner2, 1, 4, 7);
        grgrgr = (GMLReal)floweyEyeColor(siner2, 2, 4, 8);
    }

    
    float rot  = (float)(GMLReal_sin(siner / 3.0) * 4.0);
    float rotx = (float)(GMLReal_sin(siner / 4.0) * 3.0);
    float roty = (float)(GMLReal_cos(siner / 4.0) * 3.0);
    float sinS2 = (float)GMLReal_sin(siner / 2.0);
    float cosS2 = (float)GMLReal_cos(siner / 2.0);
    int32_t imgIdx = (int32_t)inst->imageIndex;
    uint32_t blend = inst->imageBlend;

    
    if (md == 0) {
        Renderer_drawSpriteExt(r, 2265, imgIdx, (inst->x + rotx * 2.0f) - 2.0f, inst->y + roty,
                               1.0f, 1.0f, rot, blend, 1.0f);
        if (desperate == 0) {
            Renderer_drawSpriteExt(r, 2267, imgIdx, (inst->x + rotx * 2.5f) - 2.0f, inst->y + roty,
                                   1.0f - sinS2 * 0.2f, 1.0f - sinS2 * 0.2f, rot, blend, 1.0f);
        } else if (desperate == 1) {
            if (frozen == 0 || frozen == 4) {
                Renderer_drawSpriteExt(r, 2267, imgIdx, (inst->x + rotx * 2.5f) - 2.0f, inst->y + roty,
                                       1.0f - sinS2 * 0.2f, 1.0f - sinS2 * 0.2f, rot, blend, 1.0f);
            } else {
                float r1 = (float)((double)rand()/(double)RAND_MAX * 3.0);
                float r2 = (float)((double)rand()/(double)RAND_MAX * 2.0);
                Renderer_drawSpriteExt(r, 2267, imgIdx, (inst->x + rotx * 2.5f) - r1, inst->y + roty + r2,
                                       0.5f - sinS2 * 0.1f, 0.5f - sinS2 * 0.1f, rot, blend, 1.0f);
            }
        } else if (desperate == 2) {
            Renderer_drawSpriteExt(r, 2267, imgIdx, (inst->x + rotx * 3.0f) - 2.0f, inst->y + roty,
                                   0.6f - sinS2 * 0.3f, 0.6f - sinS2 * 0.3f, rot, blend, 1.0f);
        }
    }
    if (md == 1) {
        Renderer_drawSpriteExt(r, 2266, imgIdx, (inst->x + rotx * 2.0f) - 2.0f, inst->y + roty,
                               1.0f, 1.0f, rot, (uint32_t)(int32_t)grgr, 1.0f);
        Renderer_drawSpriteExt(r, 2268, imgIdx, (inst->x + rotx * 2.5f) - 2.0f, inst->y + roty,
                               1.0f - sinS2 * 0.2f, 1.0f - sinS2 * 0.2f, rot,
                               (uint32_t)(int32_t)gr, 1.0f);
    }
    Renderer_drawSpriteExt(r, 2262, imgIdx, inst->x + rotx, inst->y + roty,
                           1.0f, 1.0f, rot, blend, 1.0f);

    op = 250.0;  

    
    if (md == 0) {
        Renderer_drawSpriteExt(r, 2265, imgIdx, ((inst->x + (float)op) - rotx * 2.0f) + 2.0f, inst->y + roty,
                               -1.0f, 1.0f, -rot, blend, 1.0f);
        if (desperate == 0) {
            Renderer_drawSpriteExt(r, 2267, imgIdx, ((inst->x + (float)op) - rotx * 2.5f) + 2.0f, inst->y + roty,
                                   -1.0f + cosS2 * 0.2f, 1.0f - cosS2 * 0.2f, -rot, blend, 1.0f);
        } else if (desperate == 1) {
            if (frozen == 0 || frozen == 4) {
                Renderer_drawSpriteExt(r, 2267, imgIdx, ((inst->x + (float)op) - rotx * 2.5f) + 2.0f, inst->y + roty,
                                       -1.0f + cosS2 * 0.2f, 1.0f - cosS2 * 0.2f, -rot, blend, 1.0f);
            } else {
                Renderer_drawSpriteExt(r, 2267, imgIdx, ((inst->x + (float)op) - rotx * 2.5f) + 2.0f, inst->y + roty,
                                       -0.5f + cosS2 * 0.1f, 0.5f - cosS2 * 0.1f, -rot, blend, 1.0f);
            }
        } else if (desperate == 2) {
            Renderer_drawSpriteExt(r, 2267, imgIdx, ((inst->x + (float)op) - rotx * 3.0f) + 2.0f, inst->y + roty,
                                   -0.6f + cosS2 * 0.3f, 0.6f - cosS2 * 0.3f, -rot, blend, 1.0f);
        }
    }
    if (md == 1) {
        Renderer_drawSpriteExt(r, 2266, imgIdx, ((inst->x + (float)op) - rotx * 2.0f) + 2.0f, inst->y + roty,
                               -1.0f, 1.0f, -rot, (uint32_t)(int32_t)grgr, 1.0f);
        Renderer_drawSpriteExt(r, 2268, imgIdx, ((inst->x + (float)op) - rotx * 2.5f) + 2.0f, inst->y + roty,
                               -1.0f + cosS2 * 0.2f, 1.0f - cosS2 * 0.2f, -rot,
                               (uint32_t)(int32_t)gr, 1.0f);
    }
    Renderer_drawSpriteExt(r, 2262, imgIdx, (inst->x + (float)op) - rotx, inst->y + roty,
                           -1.0f, 1.0f, -rot, blend, 1.0f);

    
    Instance_setSelfVar(inst, floweyEyeCache.con,    RValue_makeReal((GMLReal)con));
    Instance_setSelfVar(inst, floweyEyeCache.siner,  RValue_makeReal(siner));
    Instance_setSelfVar(inst, floweyEyeCache.siner2, RValue_makeReal(siner2));
    Instance_setSelfVar(inst, floweyEyeCache.md,     RValue_makeReal((GMLReal)md));
    Instance_setSelfVar(inst, floweyEyeCache.gr,     RValue_makeReal(gr));
    Instance_setSelfVar(inst, floweyEyeCache.grgr,   RValue_makeReal(grgr));
    Instance_setSelfVar(inst, floweyEyeCache.grgrgr, RValue_makeReal(grgrgr));
    Instance_setSelfVar(inst, floweyEyeCache.op,     RValue_makeReal(op));
    Instance_setSelfVar(inst, floweyEyeCache.durara, RValue_makeReal(durara));
    Instance_setSelfVar(inst, floweyEyeCache.oner,   RValue_makeReal((GMLReal)oner));
    if (floweyLeftEyeCache.rot  >= 0) Instance_setSelfVar(inst, floweyLeftEyeCache.rot,  RValue_makeReal((GMLReal)rot));
    if (floweyLeftEyeCache.rotx >= 0) Instance_setSelfVar(inst, floweyLeftEyeCache.rotx, RValue_makeReal((GMLReal)rotx));
    if (floweyLeftEyeCache.roty >= 0) Instance_setSelfVar(inst, floweyLeftEyeCache.roty, RValue_makeReal((GMLReal)roty));
}




static struct {
    int32_t siner, frozen, growth, ssx, ssy;
    bool ready;
} sidestalkCache = { .ready = false };

static void initSidestalkCache(DataWin* dw) {
    sidestalkCache.siner  = findSelfVarId(dw, "siner");
    sidestalkCache.frozen = findSelfVarId(dw, "frozen");
    sidestalkCache.growth = findSelfVarId(dw, "growth");
    sidestalkCache.ssx    = findSelfVarId(dw, "ssx");
    sidestalkCache.ssy    = findSelfVarId(dw, "ssy");
    sidestalkCache.ready = (sidestalkCache.siner >= 0 && sidestalkCache.frozen >= 0);
}

static void native_sidestalk_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!sidestalkCache.ready || runner->renderer == NULL) return;

    int32_t frozen = selfInt(inst, sidestalkCache.frozen);
    GMLReal siner = selfReal(inst, sidestalkCache.siner);
    if (frozen == 0) siner += 1.0;

    float ixs = inst->imageXscale;
    float growth = 1.0f + (float)(GMLReal_cos(siner / 5.0) * 0.01);
    float ssx = (float)(GMLReal_sin(siner / 3.0) * 2.0) * ixs;
    float ssy = (float)(GMLReal_cos(siner / 3.0) * 2.0);

    inst->imageSpeed = (frozen != 0) ? 1.0f : 3.0f;

    Instance_setSelfVar(inst, sidestalkCache.siner, RValue_makeReal(siner));
    if (sidestalkCache.growth >= 0) Instance_setSelfVar(inst, sidestalkCache.growth, RValue_makeReal(growth));
    if (sidestalkCache.ssx >= 0)    Instance_setSelfVar(inst, sidestalkCache.ssx,    RValue_makeReal(ssx));
    if (sidestalkCache.ssy >= 0)    Instance_setSelfVar(inst, sidestalkCache.ssy,    RValue_makeReal(ssy));

    Renderer_drawSpriteExt(runner->renderer, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x - ssx, inst->y + ssy, ixs, growth, 0.0f,
                           inst->imageBlend, 1.0f);
}






static struct {
    int32_t offon, bonus;
    bool ready;
} spinbulletPrevCache = { .ready = false };

static void initSpinbulletPrevCache(DataWin* dw) {
    spinbulletPrevCache.offon = findSelfVarId(dw, "offon");
    spinbulletPrevCache.bonus = findSelfVarId(dw, "bonus");
    spinbulletPrevCache.ready = (spinbulletPrevCache.offon >= 0);
}

static void native_spinbulletPrev_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!spinbulletPrevCache.ready) return;
    Renderer* r = runner->renderer;

    int32_t offon = selfInt(inst, spinbulletPrevCache.offon) + 1;
    if (offon > 2) offon = 0;
    Instance_setSelfVar(inst, spinbulletPrevCache.offon, RValue_makeReal((GMLReal)offon));

    
    
    inst->imageAlpha += 0.334f;

    
    
    uint32_t col;
    if (offon == 0)      col = 0x0000FFu;
    else if (offon == 1) col = 0x40A0FFu;
    else                 col = 0x00FFFFu;
    
    if (inst->imageAlpha > 6.0f) col = 0xFFFFFFu;

    
    if (r != NULL && r->vtable->drawCircle != NULL) {
        float circleAlpha = inst->imageAlpha;
        if (circleAlpha > 1.0f) circleAlpha = 1.0f;
        int32_t savedPrec = r->circlePrecision;
        r->circlePrecision = 24;
        r->vtable->drawCircle(r, inst->x, inst->y, 60.0f, col, circleAlpha, true, 24);
        r->circlePrecision = savedPrec;
    }

    GMLReal bonus = (spinbulletPrevCache.bonus >= 0) ? selfReal(inst, spinbulletPrevCache.bonus) : 0.0;
    if (inst->imageAlpha > (float)(8.0 + bonus)) {
        Runner_createInstance(runner, inst->x, inst->y, 1652);
        Runner_destroyInstance(runner, inst);
    }
}




static struct {
    int32_t active, onoff, xxl, yyl, nowtime, maxtime, memorymode, visible;
    bool ready;
} gigavinePrevCache = { .ready = false };

static void initGigavinePrevCache(DataWin* dw) {
    gigavinePrevCache.active  = findSelfVarId(dw, "active");
    gigavinePrevCache.onoff   = findSelfVarId(dw, "onoff");
    gigavinePrevCache.xxl     = findSelfVarId(dw, "xxl");
    gigavinePrevCache.yyl     = findSelfVarId(dw, "yyl");
    gigavinePrevCache.nowtime = findSelfVarId(dw, "nowtime");
    gigavinePrevCache.maxtime = findSelfVarId(dw, "maxtime");
    gigavinePrevCache.memorymode = findSelfVarId(dw, "memorymode");
    gigavinePrevCache.ready = (gigavinePrevCache.active >= 0 && gigavinePrevCache.onoff >= 0 &&
                               gigavinePrevCache.nowtime >= 0 && gigavinePrevCache.maxtime >= 0);
}

static void native_gigavinePrev_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!gigavinePrevCache.ready || runner->renderer == NULL) return;
    if (selfInt(inst, gigavinePrevCache.active) != 1) return;
    Renderer* r = runner->renderer;

    int32_t onoff = selfInt(inst, gigavinePrevCache.onoff) + 1;
    if (onoff > 2) onoff = 0;
    
    
    uint32_t col;
    if (onoff == 0)      col = 0x0000FFu;
    else if (onoff == 1) col = 0x40A0FFu;
    else                 col = 0x00FFFFu;

    float dirRad = inst->direction * (float)(M_PI / 180.0);
    float xxl = 600.0f * GMLReal_cos(dirRad);
    float yyl = -600.0f * GMLReal_sin(dirRad);  

    r->vtable->drawLine(r, inst->x - 8.0f, inst->y,
                        (inst->x + xxl) - 8.0f, inst->y + yyl, 2.0f, col, 1.0f);
    r->vtable->drawLine(r, inst->x + 8.0f, inst->y,
                        inst->x + xxl + 8.0f, inst->y + yyl, 2.0f, col, 1.0f);

    GMLReal nowtime = selfReal(inst, gigavinePrevCache.nowtime) + 1.0;
    GMLReal maxtime = selfReal(inst, gigavinePrevCache.maxtime);

    Instance_setSelfVar(inst, gigavinePrevCache.onoff, RValue_makeReal((GMLReal)onoff));
    Instance_setSelfVar(inst, gigavinePrevCache.nowtime, RValue_makeReal(nowtime));
    if (gigavinePrevCache.xxl >= 0) Instance_setSelfVar(inst, gigavinePrevCache.xxl, RValue_makeReal((GMLReal)xxl));
    if (gigavinePrevCache.yyl >= 0) Instance_setSelfVar(inst, gigavinePrevCache.yyl, RValue_makeReal((GMLReal)yyl));

    if (nowtime > maxtime) {
        inst->imageAngle = inst->direction;
        Instance* gv = Runner_createInstance(runner, inst->x, inst->y, 1643);
        if (gv) {
            if (gigavinePrevCache.memorymode >= 0)
                Instance_setSelfVar(gv, gigavinePrevCache.memorymode,
                                    Instance_getSelfVar(inst, gigavinePrevCache.memorymode));
            gv->imageAngle = inst->imageAngle;
        }
        int32_t memorymode = (gigavinePrevCache.memorymode >= 0)
                             ? selfInt(inst, gigavinePrevCache.memorymode) : 0;
        if (memorymode == 0) {
            Runner_destroyInstance(runner, inst);
        } else {
            Instance_setSelfVar(inst, gigavinePrevCache.active, RValue_makeReal(0.0));
            inst->visible = false;
        }
    }
}




static struct {
    int32_t desperate, acon, acon2, frozen, siner, growth, ssx, ssy, xr;
    int32_t reach, reach2, reach3, made, venu;
    bool ready;
} floweyArmCache = { .ready = false };

static void initFloweyArmCache(DataWin* dw) {
    floweyArmCache.desperate = findSelfVarId(dw, "desperate");
    floweyArmCache.acon      = findSelfVarId(dw, "acon");
    floweyArmCache.acon2     = findSelfVarId(dw, "acon2");
    floweyArmCache.frozen    = findSelfVarId(dw, "frozen");
    floweyArmCache.siner     = findSelfVarId(dw, "siner");
    floweyArmCache.growth    = findSelfVarId(dw, "growth");
    floweyArmCache.ssx       = findSelfVarId(dw, "ssx");
    floweyArmCache.ssy       = findSelfVarId(dw, "ssy");
    floweyArmCache.xr        = findSelfVarId(dw, "xr");
    floweyArmCache.reach     = findSelfVarId(dw, "reach");
    floweyArmCache.reach2    = findSelfVarId(dw, "reach2");
    floweyArmCache.reach3    = findSelfVarId(dw, "reach3");
    floweyArmCache.made      = findSelfVarId(dw, "made");
    floweyArmCache.venu      = findSelfVarId(dw, "venu");
    floweyArmCache.ready = (floweyArmCache.desperate >= 0 && floweyArmCache.acon >= 0 &&
                            floweyArmCache.siner >= 0 && floweyArmCache.reach >= 0);
}

static void native_floweyArm_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!floweyArmCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t desperate = selfInt(inst, floweyArmCache.desperate);
    int32_t acon = selfInt(inst, floweyArmCache.acon);
    int32_t acon2 = selfInt(inst, floweyArmCache.acon2);
    int32_t frozen = selfInt(inst, floweyArmCache.frozen);
    GMLReal siner = selfReal(inst, floweyArmCache.siner);
    GMLReal reach = selfReal(inst, floweyArmCache.reach);
    GMLReal reach2 = selfReal(inst, floweyArmCache.reach2);
    int32_t made = selfInt(inst, floweyArmCache.made);
    float ixs = inst->imageXscale;
    int32_t imgIdx = (int32_t)inst->imageIndex;

    
    if ((desperate == 0 || desperate == 1) && acon == 0) {
        if (frozen == 0) siner += (desperate == 1) ? 1.2 : 0.8;
        float growth = 1.0f + (float)(GMLReal_cos(siner / 6.0) * 0.03);
        float mulssx = (desperate == 1) ? 5.0f : 4.0f;
        float mulssy = (desperate == 1) ? 8.0f : 4.0f;
        float ssx = (float)(GMLReal_sin(siner / 3.0) * (double)mulssx) * ixs;
        float ssy = (float)(GMLReal_cos(siner / 3.0) * (double)mulssy);
        float yOffset = (desperate == 1) ? 2.0f : 0.0f;
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx,
                               inst->x - ssx, inst->y + ssy + yOffset,
                               ixs, growth, 0.0f, inst->imageBlend, 1.0f);
        if (floweyArmCache.growth >= 0) Instance_setSelfVar(inst, floweyArmCache.growth, RValue_makeReal(growth));
        if (floweyArmCache.ssx >= 0)    Instance_setSelfVar(inst, floweyArmCache.ssx,    RValue_makeReal(ssx));
        if (floweyArmCache.ssy >= 0)    Instance_setSelfVar(inst, floweyArmCache.ssy,    RValue_makeReal(ssy));
    }

    float xr = -ixs;
    if (floweyArmCache.xr >= 0) Instance_setSelfVar(inst, floweyArmCache.xr, RValue_makeReal((GMLReal)xr));

    
    if (acon == 2) {
        Renderer_drawSpriteExt(r, 2339, (int32_t)reach, inst->x, inst->y,
                               ixs, inst->imageYscale, inst->imageAngle,
                               inst->imageBlend, inst->imageAlpha);
        if (reach > 0.0) reach -= 1.0;
        if (reach <= 0.0) {
            acon = 0; reach = 0.0; reach2 = 0.0;
            acon2 = 0; made = 0;
            if (floweyArmCache.reach3 >= 0)
                Instance_setSelfVar(inst, floweyArmCache.reach3, RValue_makeReal(0.0));
        }
    }

    
    if (acon == 1) {
        if (acon2 == 1 || acon2 == 3) {
            if (reach2 < 13.0) reach2 += 2.0;
            Renderer_drawSpriteExt(r, 2338, (int32_t)reach2,
                                   inst->x + (xr * 36.0f), inst->y + 195.0f,
                                   ixs, inst->imageYscale, inst->imageAngle,
                                   inst->imageBlend, inst->imageAlpha);
        }
        Renderer_drawSpriteExt(r, 2339, (int32_t)reach, inst->x, inst->y,
                               ixs, inst->imageYscale, inst->imageAngle,
                               inst->imageBlend, inst->imageAlpha);
        if (reach < 11.0) reach += 2.0;
        if (reach == 12.0) reach = 11.0;
        else if (acon2 == 0) acon2 = 1;

        if (acon2 == 1) {
            if (reach2 == 14.0) reach2 = 13.0;
            if (reach2 == 13.0 && made == 0) {
                made = 1;
                
                Instance* venu = Runner_createInstance(runner, inst->x - 135.0f * xr, inst->y + 138.0f, 1658);
                if (venu) {
                    if (floweyArmCache.venu >= 0)
                        Instance_setSelfVar(inst, floweyArmCache.venu,
                                            RValue_makeReal((GMLReal)venu->instanceId));
                    
                    int32_t bossVar = findSelfVarId(ctx->dataWin, "boss");
                    if (bossVar >= 0)
                        Instance_setSelfVar(venu, bossVar, RValue_makeReal((GMLReal)inst->instanceId));
                    int32_t siderVar = findSelfVarId(ctx->dataWin, "sider");
                    if (ixs > 0.0f) {
                        if (siderVar >= 0) Instance_setSelfVar(venu, siderVar, RValue_makeReal(1.0));
                        venu->imageXscale = -1.0f;
                    } else if (ixs < 0.0f) {
                        if (siderVar >= 0) Instance_setSelfVar(venu, siderVar, RValue_makeReal(0.0));
                    }
                }
            }
        }
        if (acon2 == 3) reach2 += 1.0;
        if (reach2 >= 39.0) acon = 2;
    }

    
    Instance_setSelfVar(inst, floweyArmCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, floweyArmCache.acon,  RValue_makeReal((GMLReal)acon));
    Instance_setSelfVar(inst, floweyArmCache.acon2, RValue_makeReal((GMLReal)acon2));
    Instance_setSelfVar(inst, floweyArmCache.reach, RValue_makeReal(reach));
    Instance_setSelfVar(inst, floweyArmCache.reach2, RValue_makeReal(reach2));
    Instance_setSelfVar(inst, floweyArmCache.made,  RValue_makeReal((GMLReal)made));
}






static struct {
    int32_t tvmode, anim, animspeed, animchoice, animtimer;
    int32_t anim3, anim4, anim5, anim6;
    int32_t siner, size, tt;
    int32_t overnoiser, shudder, shuddercounter, flasheron, remx;
    int32_t gSoulRescue, gFaceemotion;
    bool ready;
} floweyTvCache = { .ready = false };

static void initFloweyTvCache(VMContext* ctx, DataWin* dw) {
    floweyTvCache.tvmode    = findSelfVarId(dw, "tvmode");
    floweyTvCache.anim      = findSelfVarId(dw, "anim");
    floweyTvCache.animspeed = findSelfVarId(dw, "animspeed");
    floweyTvCache.animchoice = findSelfVarId(dw, "animchoice");
    floweyTvCache.animtimer = findSelfVarId(dw, "animtimer");
    floweyTvCache.anim3     = findSelfVarId(dw, "anim3");
    floweyTvCache.anim4     = findSelfVarId(dw, "anim4");
    floweyTvCache.anim5     = findSelfVarId(dw, "anim5");
    floweyTvCache.anim6     = findSelfVarId(dw, "anim6");
    floweyTvCache.siner     = findSelfVarId(dw, "siner");
    floweyTvCache.size      = findSelfVarId(dw, "size");
    floweyTvCache.tt        = findSelfVarId(dw, "tt");
    floweyTvCache.overnoiser = findSelfVarId(dw, "overnoiser");
    floweyTvCache.shudder   = findSelfVarId(dw, "shudder");
    floweyTvCache.shuddercounter = findSelfVarId(dw, "shuddercounter");
    floweyTvCache.flasheron = findSelfVarId(dw, "flasheron");
    floweyTvCache.remx      = findSelfVarId(dw, "remx");
    floweyTvCache.gSoulRescue  = findGlobalVarId(ctx, "soul_rescue");
    floweyTvCache.gFaceemotion = findGlobalVarId(ctx, "faceemotion");
    floweyTvCache.ready = (floweyTvCache.tvmode >= 0 && floweyTvCache.siner >= 0 &&
                           floweyTvCache.size >= 0 && floweyTvCache.anim >= 0 &&
                           floweyTvCache.gSoulRescue >= 0);
}


static inline float tvRand(float max) {
    return (float)((double)rand() / (double)RAND_MAX * (double)max);
}

static void native_floweyTv_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyTvCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t tvmode = selfInt(inst, floweyTvCache.tvmode);
    GMLReal anim = selfReal(inst, floweyTvCache.anim);
    GMLReal animspeed = selfReal(inst, floweyTvCache.animspeed);
    GMLReal animchoice = selfReal(inst, floweyTvCache.animchoice);
    GMLReal animtimer = selfReal(inst, floweyTvCache.animtimer);
    GMLReal anim3 = selfReal(inst, floweyTvCache.anim3);
    GMLReal anim4 = selfReal(inst, floweyTvCache.anim4);
    GMLReal anim5 = selfReal(inst, floweyTvCache.anim5);
    GMLReal anim6 = selfReal(inst, floweyTvCache.anim6);
    GMLReal siner = selfReal(inst, floweyTvCache.siner);
    GMLReal size = selfReal(inst, floweyTvCache.size);
    GMLReal tt = selfReal(inst, floweyTvCache.tt);
    int32_t overnoiser = selfInt(inst, floweyTvCache.overnoiser);
    GMLReal shudder = selfReal(inst, floweyTvCache.shudder);
    GMLReal shuddercounter = selfReal(inst, floweyTvCache.shuddercounter);
    int32_t flasheron = selfInt(inst, floweyTvCache.flasheron);
    GMLReal remx = selfReal(inst, floweyTvCache.remx);
    GMLReal soulRescue = globalReal(ctx, floweyTvCache.gSoulRescue);
    GMLReal faceEmotion = (floweyTvCache.gFaceemotion >= 0) ? globalReal(ctx, floweyTvCache.gFaceemotion) : 0.0;
    int32_t imgIdx = (int32_t)inst->imageIndex;
    uint32_t blend = inst->imageBlend;

    float fx = inst->x, fy = inst->y;
    float rx2 = 0, rx4 = 0, rx5 = 0;  

    switch (tvmode) {
      case 0: {
        Renderer_drawSpriteExt(r, 2310, (int32_t)anim,
                               fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                               (float)size + 0.9f + tvRand(0.1f),
                               (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, 1.0f);
        anim += animspeed;
        if (anim > (animchoice + 1.0)) animspeed = -animspeed;
        if (anim < (animchoice - 1.0)) animspeed = -animspeed;
        animtimer += 1.0;
        if (animtimer > 100.0) {
            Renderer_drawSpriteExt(r, 2309, (int32_t)tvRand(3.0f),
                                   fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                                   (float)size + 0.8f + tvRand(0.1f),
                                   (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, 1.0f);
            animchoice = (GMLReal)(int32_t)(tvRand(100.0f) + 0.5f);  
            anim = animchoice;
            if (animtimer > 106.0) animtimer = 0.0;
        }
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
        siner += 1.0;
      } break;

      case 1: {
        inst->alarm[1] = -1;
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 10.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        float sprAlpha = 0.8f + (float)GMLReal_sin(siner / 2.0);
        if (sprAlpha < 0.0f) sprAlpha = 0.0f; if (sprAlpha > 1.0f) sprAlpha = 1.0f;
        Renderer_drawSpriteExt(r, 2363, 0, fx + 20.0f, fy + 50.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, sprAlpha);
        Renderer_drawSprite(r, 2364, (int32_t)soulRescue,
                            fx + 80.0f + tvRand(3.0f), fy + 110.0f + tvRand(3.0f));
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
        siner += 1.0;
      } break;

      case 2: {
        inst->alarm[1] = -1;
        Renderer_drawSpriteExt(r, 2309, (int32_t)tvRand(3.0f), fx + 26.0f, fy + 50.0f,
                               1.2f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
      } break;

      case 3: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 10.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        siner += 1.0;
        Renderer_drawSprite(r, 2364, (int32_t)soulRescue,
                            fx + 80.0f, fy + 90.0f + (float)(GMLReal_sin(siner / 8.0) * 3.0));
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
      } break;

      case 4: {
        Renderer_drawSpriteExt(r, 2310, (int32_t)tt,
                               fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                               (float)size + 0.9f + tvRand(0.1f),
                               (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
      } break;

      case 5: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy);
      } break;

      case 10: {
        Renderer_drawSpriteExt(r, 2308, (int32_t)tt, fx + 13.0f, fy + 50.0f,
                               1.0f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy);
      } break;

      case 11: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        Renderer_drawSpriteExt(r, 2299, (int32_t)GMLReal_floor(faceEmotion),
                               fx + 21.0f + tvRand(2.0f), fy + 56.0f + tvRand(2.0f),
                               2.9f + tvRand(0.1f), 2.9f + tvRand(0.1f), 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy);
      } break;

      case 12: {
        Renderer_drawSpriteExt(r, 2310, 35, fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                               (float)size + 0.9f + tvRand(0.1f),
                               (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, 1.0f);
        anim4 = 0.0;
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
      } break;

      case 13: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        float a = 1.0f - (float)anim4;
        if (a < 0.0f) a = 0.0f; if (a > 1.0f) a = 1.0f;
        Renderer_drawSpriteExt(r, 2310, 35, fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                               (float)size + 0.9f + tvRand(0.1f),
                               (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, a);
        anim4 += 0.01;
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
        siner += 1.0;
      } break;

      case 18: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, 1.0f, false);
        siner += 1.0;
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy);
        Renderer_drawSpriteExt(r, 2304, 0, fx + 25.0f, fy + 65.0f, 1.3f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
      } break;

      case 19: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, 1.0f, false);
        siner += 1.0;
        Renderer_drawSpriteExt(r, 2304, (int32_t)GMLReal_floor(anim4),
                               fx + 25.0f, fy + 65.0f, 1.3f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, fx, fy, 1.0f, 1.0f, 0.0f, blend, 1.0f);
        anim5 += 0.5;
        if (anim4 < 6.0) anim4 += 0.5;
        if (anim5 > 22.0 && anim4 < 15.0) anim4 += 0.5;
      } break;

      case 20: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, 1.0f, false);
        siner += 1.0;
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin((siner * M_PI) / 2.0) * 2.0));
        Renderer_drawSpriteExt(r, 2305, (int32_t)GMLReal_floor(anim3),
                               fx + 25.0f, fy + 65.0f, 1.3f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        anim3 += 0.5;
      } break;

      case 21: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, 1.0f, false);
        siner += 1.0;
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 2.0));
        Renderer_drawSpriteExt(r, 2307, 0, fx + 25.0f, fy + 65.0f + (float)GMLReal_sin(siner / 3.0),
                               1.3f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
      } break;

      case 22: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, 1.0f, false);
        siner += 1.0;
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy);
        Renderer_drawSpriteExt(r, 2306, (int32_t)faceEmotion,
                               fx + 25.0f + tvRand(2.0f), fy + 65.0f + tvRand(2.0f),
                               1.3f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
      } break;

      case 24: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        float fadeA = (float)((anim6 - 30.0) / 30.0);
        if (fadeA < 0.0f) fadeA = 0.0f; if (fadeA > 1.0f) fadeA = 1.0f;
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, fadeA, false);
        anim6 += 1.0;
        Renderer_drawSpriteExt(r, 2304, 0, fx + 25.0f, fy + 65.0f, 1.3f, 1.0f, 0.0f, 0xFFFFFFu, fadeA);
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, fx, fy, 1.0f, 1.0f, 0.0f, blend, 1.0f);
      } break;

      case 25: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, fx, fy, 1.0f, 1.0f, 0.0f, blend, 1.0f);
      } break;

      case 26: {
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0x000000u, 1.0f, false);
        float fadeA = (float)((anim6 - 11.0) / 10.0);
        if (fadeA < 0.0f) fadeA = 0.0f; if (fadeA > 1.0f) fadeA = 1.0f;
        r->vtable->drawRectangle(r, fx + 20.0f, fy + 50.0f, fx + 160.0f, fy + 140.0f,
                                 0xFFFFFFu, fadeA, false);
        anim6 += 1.0;
        Renderer_drawSpriteExt(r, 2304, 11, fx + 25.0f, fy + 65.0f, 1.3f, 1.0f, 0.0f, 0xFFFFFFu, fadeA);
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, fx, fy, 1.0f, 1.0f, 0.0f, blend, 1.0f);
      } break;

      case 99: {
        Renderer_drawSpriteExt(r, 2310, (int32_t)anim,
                               fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                               (float)size + 0.9f + tvRand(0.1f),
                               (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, 1.0f);
        float jx = tvRand(3.0f) - tvRand(3.0f);
        float jy = tvRand(3.0f) - tvRand(3.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx + jx, fy + jy);
        anim += 0.5;
      } break;

      case 100: {
        Renderer_drawSpriteExt(r, 2310, (int32_t)anim,
                               fx + 20.0f + tvRand(4.0f), fy + 50.0f + tvRand(4.0f),
                               (float)size + 0.9f + tvRand(0.1f),
                               (float)size + tvRand(0.1f) + 0.4f, 0.0f, 0xFFFFFFu, 1.0f);
        rx5 = tvRand(5.0f) - tvRand(5.0f);
        float ry5 = tvRand(5.0f) - tvRand(5.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx + rx5, fy + ry5);
        anim += 1.0;
      } break;

      default: break;
    }

    
    if (overnoiser > 0) {
        Renderer_drawSpriteExt(r, 2309, (int32_t)tvRand(3.0f), fx + 26.0f, fy + 50.0f,
                               1.2f, 1.0f, 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSprite(r, inst->spriteIndex, imgIdx, fx, fy);
        overnoiser -= 1;
    }

    if (shudder > 0.0) {
        shuddercounter += 1.0;
        if (shuddercounter > 0.0) {
            if (flasheron == 1) {
                Renderer_drawSprite(r, 2382, imgIdx, fx, fy + (float)(GMLReal_sin(siner / 3.0) * 1.0));
                flasheron = 0;
            } else {
                flasheron = 1;
            }
            rx2 = tvRand((float)shudder) - tvRand((float)shudder);
            float ry2 = tvRand((float)shudder) - tvRand((float)shudder);
            inst->x = (float)(remx + rx2);
            inst->y = inst->ystart + ry2;
            shudder -= 2.0;
            shuddercounter = 0.0;
        }
        if (shudder < 1.0) shudder = 0.0;
    }

    
    (void)rx2; (void)rx4; (void)rx5;

    
    Instance_setSelfVar(inst, floweyTvCache.anim,      RValue_makeReal(anim));
    Instance_setSelfVar(inst, floweyTvCache.animspeed, RValue_makeReal(animspeed));
    Instance_setSelfVar(inst, floweyTvCache.animchoice, RValue_makeReal(animchoice));
    Instance_setSelfVar(inst, floweyTvCache.animtimer, RValue_makeReal(animtimer));
    Instance_setSelfVar(inst, floweyTvCache.anim3, RValue_makeReal(anim3));
    Instance_setSelfVar(inst, floweyTvCache.anim4, RValue_makeReal(anim4));
    Instance_setSelfVar(inst, floweyTvCache.anim5, RValue_makeReal(anim5));
    Instance_setSelfVar(inst, floweyTvCache.anim6, RValue_makeReal(anim6));
    Instance_setSelfVar(inst, floweyTvCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, floweyTvCache.overnoiser, RValue_makeReal((GMLReal)overnoiser));
    Instance_setSelfVar(inst, floweyTvCache.shudder,    RValue_makeReal(shudder));
    Instance_setSelfVar(inst, floweyTvCache.shuddercounter, RValue_makeReal(shuddercounter));
    Instance_setSelfVar(inst, floweyTvCache.flasheron, RValue_makeReal((GMLReal)flasheron));
}







#define HANDGUN_MAX_CON_VARIDS 16
static struct {
    int32_t conVarIds[HANDGUN_MAX_CON_VARIDS];
    int32_t conCount;
    bool ready;
} handgunCache = { .ready = false };

static void initHandgunCache(DataWin* dw) {
    handgunCache.conCount = findAllSelfVarIds(dw, "con", handgunCache.conVarIds, HANDGUN_MAX_CON_VARIDS);
    handgunCache.ready = (handgunCache.conCount > 0);
}

static void native_handgun_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    Renderer_drawSpriteExt(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, inst->imageXscale, inst->imageYscale, 0.0f,
                           0xFFFFFFu, 1.0f);
    if (handgunCache.ready) {
        
        
        
        
        
        
        for (int32_t i = 0; i < handgunCache.conCount; i++) {
            RValue v = Instance_getSelfVar(inst, handgunCache.conVarIds[i]);
            if (v.type == RVALUE_UNDEFINED) continue;
            GMLReal con = RValue_toReal(v);
            float diff = (float)con - 2.1f; if (diff < 0.0f) diff = -diff;
            if (diff < 0.05f) {
                
                r->vtable->drawLine(r, 0.0f, inst->y, 700.0f, inst->y, 1.0f, 0x0000FFu, 1.0f);
                break;
            }
        }
    }
}




static void native_venusPl_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (runner->renderer == NULL) return;
    Renderer_drawSpriteExt(runner->renderer, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, inst->imageXscale, inst->imageYscale, 0.0f,
                           0xFFFFFFu, inst->imageAlpha);
}




static struct { int32_t siner, frozen, desperate; bool ready; } fleshfaceCache = { .ready = false };

static void initFleshfaceCache(DataWin* dw) {
    fleshfaceCache.siner     = findSelfVarId(dw, "siner");
    fleshfaceCache.frozen    = findSelfVarId(dw, "frozen");
    fleshfaceCache.desperate = findSelfVarId(dw, "desperate");
    fleshfaceCache.ready = (fleshfaceCache.siner >= 0 && fleshfaceCache.frozen >= 0);
}

static void native_fleshface_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!fleshfaceCache.ready || runner->renderer == NULL) return;

    int32_t frozen = selfInt(inst, fleshfaceCache.frozen);
    GMLReal siner = selfReal(inst, fleshfaceCache.siner);
    if (frozen == 0) siner += 1.2;

    int32_t desperate = selfInt(inst, fleshfaceCache.desperate);
    Renderer* r = runner->renderer;
    int32_t imgIdx = (int32_t)inst->imageIndex;
    uint32_t blend = inst->imageBlend;

    if (desperate == 0) {
        siner -= 0.2;
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, inst->x,
                               inst->y + (float)(GMLReal_sin(siner / 2.0) * 3.0),
                               1.0f, 1.0f, 0.0f, blend, 1.0f);
        Renderer_drawSpriteExt(r, 2260, (int32_t)(siner / 8.0), inst->x + 32.0f,
                               inst->y + 12.0f + (float)(GMLReal_sin(siner / 2.0) * 6.0),
                               1.0f, 1.0f, 0.0f, blend, 1.0f);
    } else if (desperate == 1) {
        Renderer_drawSpriteExt(r, inst->spriteIndex, imgIdx, inst->x,
                               inst->y + (float)(GMLReal_sin(siner / 2.0) * 3.0),
                               1.0f, 1.0f, 0.0f, blend, 1.0f);
        Renderer_drawSpriteExt(r, 2260, (int32_t)(siner / 8.0), inst->x + 32.0f,
                               inst->y + 14.0f + (float)(GMLReal_sin(siner / 2.0) * 7.0),
                               1.0f, 1.0f, 0.0f, blend, 1.0f);
    }
    Instance_setSelfVar(inst, fleshfaceCache.siner, RValue_makeReal(siner));
}




static struct {
    int32_t siner, size, halfsies, maxer;
    int32_t dnty, dntyx, dnty2, rt, ssx, ssy;
    bool ready;
} dentataCache = { .ready = false };

static void initDentataCache(DataWin* dw) {
    dentataCache.siner    = findSelfVarId(dw, "siner");
    dentataCache.size     = findSelfVarId(dw, "size");
    dentataCache.halfsies = findSelfVarId(dw, "halfsies");
    dentataCache.maxer    = findSelfVarId(dw, "maxer");
    dentataCache.dnty  = findSelfVarId(dw, "dnty");
    dentataCache.dntyx = findSelfVarId(dw, "dntyx");
    dentataCache.dnty2 = findSelfVarId(dw, "dnty2");
    dentataCache.rt    = findSelfVarId(dw, "rt");
    dentataCache.ssx   = findSelfVarId(dw, "ssx");
    dentataCache.ssy   = findSelfVarId(dw, "ssy");
    dentataCache.ready = (dentataCache.siner >= 0 && dentataCache.size >= 0 &&
                          dentataCache.halfsies >= 0 && dentataCache.maxer >= 0);
}

static void native_dentataFull_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!dentataCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, dentataCache.siner);
    GMLReal size  = selfReal(inst, dentataCache.size);
    int32_t halfsies = selfInt(inst, dentataCache.halfsies);
    float ixs = inst->imageXscale;

    float dnty  = (float)(GMLReal_sin(siner / 4.0) * 2.0 * size);
    float dntyx = (float)(GMLReal_cos(siner / 2.0) * 2.0 * size);
    float dnty2 = (float)(GMLReal_sin(siner / 3.0) * 4.0 * size);
    float rt    = (float)(GMLReal_cos(siner / 4.0) * 5.0);
    float ssx   = (float)(GMLReal_sin(siner / 3.0) * 3.0) * ixs;
    float ssy   = (float)(GMLReal_cos(siner / 3.0) * 3.0);
    float fSize = (float)size;

    if (halfsies == 0) {
        Renderer_drawSpriteExt(r, 2290, 0, inst->x + ssx, inst->y + dnty + ssy,
                               fSize, fSize, rt, 0x808080u, 1.0f);
        Renderer_drawSpriteExt(r, 2288, 0, (inst->x - dntyx) + ssx,
                               ((inst->y + dnty2) - 4.0f * fSize) + ssy,
                               fSize, fSize, rt, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 2289, 0, inst->x + dntyx,
                               (inst->y - dnty2) + 4.0f * fSize,
                               fSize, fSize, rt, 0xFFFFFFu, 1.0f);
    } else if (halfsies == 1) {
        rt += 90.0f;
        Renderer_drawSpriteExt(r, 2284, 0, inst->x + dnty + ssx, inst->y + ssy,
                               fSize, fSize, rt, 0x808080u, 1.0f);
        Renderer_drawSpriteExt(r, 2287, 0, ((inst->x + dnty2) - 4.0f * fSize) + ssx,
                               (inst->y - dntyx) + ssy, fSize, fSize, rt, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 2286, 0, (inst->x - dnty2) + 4.0f * fSize + ssx,
                               inst->y + dntyx + ssy, fSize, fSize, rt, 0xFFFFFFu, 1.0f);
    } else if (halfsies == 2) {
        rt -= 90.0f;
        Renderer_drawSpriteExt(r, 2284, 0, inst->x + dnty + ssx, inst->y + ssy,
                               fSize, fSize, rt, 0x808080u, 1.0f);
        Renderer_drawSpriteExt(r, 2287, 0, (inst->x - dnty2) + 4.0f * fSize + ssx,
                               (inst->y - dntyx) + ssy, fSize, fSize, rt, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 2286, 0, ((inst->x + dnty2) - 4.0f * fSize) + ssx,
                               inst->y + dntyx + ssy, fSize, fSize, rt, 0xFFFFFFu, 1.0f);
    }

    siner += 1.0;
    GMLReal maxer = selfReal(inst, dentataCache.maxer);
    if (maxer < 0.8) maxer += 0.1;
    size = maxer + GMLReal_sin(siner / 2.0) * 0.02;

    Instance_setSelfVar(inst, dentataCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, dentataCache.maxer, RValue_makeReal(maxer));
    Instance_setSelfVar(inst, dentataCache.size,  RValue_makeReal(size));
    if (dentataCache.dnty  >= 0) Instance_setSelfVar(inst, dentataCache.dnty,  RValue_makeReal((GMLReal)dnty));
    if (dentataCache.dntyx >= 0) Instance_setSelfVar(inst, dentataCache.dntyx, RValue_makeReal((GMLReal)dntyx));
    if (dentataCache.dnty2 >= 0) Instance_setSelfVar(inst, dentataCache.dnty2, RValue_makeReal((GMLReal)dnty2));
    if (dentataCache.rt    >= 0) Instance_setSelfVar(inst, dentataCache.rt,    RValue_makeReal((GMLReal)rt));
    if (dentataCache.ssx   >= 0) Instance_setSelfVar(inst, dentataCache.ssx,   RValue_makeReal((GMLReal)ssx));
    if (dentataCache.ssy   >= 0) Instance_setSelfVar(inst, dentataCache.ssy,   RValue_makeReal((GMLReal)ssy));
}




static struct {
    int32_t counter, word, factor, type, shake;
    bool ready;
} wordbulletCache = { .ready = false };

static void initWordbulletCache(DataWin* dw) {
    wordbulletCache.counter = findSelfVarId(dw, "counter");
    wordbulletCache.word    = findSelfVarId(dw, "word");
    wordbulletCache.factor  = findSelfVarId(dw, "factor");
    wordbulletCache.type    = findSelfVarId(dw, "type");
    wordbulletCache.shake   = findSelfVarId(dw, "shake");
    wordbulletCache.ready = (wordbulletCache.counter >= 0 && wordbulletCache.word >= 0);
}

static void native_wordbullet_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!wordbulletCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal counter = selfReal(inst, wordbulletCache.counter) + 1.0;
    Instance_setSelfVar(inst, wordbulletCache.counter, RValue_makeReal(counter));

    
    int32_t gLang = findGlobalVarId(ctx, "language");
    bool isJa = false;
    if (gLang >= 0) {
        RValue langVal = ctx->globalVars[gLang];
        if (langVal.type == RVALUE_STRING && langVal.string &&
            strcmp(langVal.string, "ja") == 0) isJa = true;
    }
    r->drawFont = isJa ? 14 : 2;

    RValue wordVal = Instance_getSelfVar(inst, wordbulletCache.word);
    const char* word = (wordVal.type == RVALUE_STRING && wordVal.string) ? wordVal.string : "";

    float widthF = 0.0f;
    BuiltinFunc swFn = VMBuiltins_find("string_width");
    if (swFn) {
        RValue arg[1] = { wordVal };
        RValue sw = swFn(ctx, arg, 1);
        widthF = (float)RValue_toReal(sw);
        RValue_free(&sw);
    }
    float factor = (widthF > 0.0f) ? (100.0f / widthF) : 1.0f;
    Instance_setSelfVar(inst, wordbulletCache.factor, RValue_makeReal((GMLReal)factor));

    uint32_t savedColor = r->drawColor;
    r->drawColor = inst->imageBlend;
    r->vtable->drawText(r, word, inst->x, inst->y, factor, 4.2f, 0.0f);
    r->drawColor = savedColor;

    
    if (inst->hspeed > 0.0f && inst->x > 405.0f) { Runner_destroyInstance(runner, inst); return; }
    if (inst->hspeed < 0.0f && inst->x < 120.0f) { Runner_destroyInstance(runner, inst); return; }

    if (wordbulletCache.type >= 0 && selfInt(inst, wordbulletCache.type) == 2) {
        GMLReal shake = selfReal(inst, wordbulletCache.shake) + 0.2;
        GMLReal jx = ((double)rand()/(double)RAND_MAX - (double)rand()/(double)RAND_MAX) * shake;
        GMLReal jy = ((double)rand()/(double)RAND_MAX - (double)rand()/(double)RAND_MAX) * shake;
        inst->x += (float)jx;
        inst->y += (float)jy;
        Instance_setSelfVar(inst, wordbulletCache.shake, RValue_makeReal(shake));
    }
}















#if 0
#define ACT_MAX_CON_VARIDS 16
typedef struct {
    
    int32_t con;        
    int32_t type;       
    
    
    
    
    const int32_t* conCandidates;
    int32_t conCandidateCount;
    
    float ox1, oy1, ox2, oy2;
    float ix1, iy1, ix2, iy2;
    float writerX, writerY;
    
    const char* msg0Key;
    const char* msg1Key;
    
    float alphaDec;
    
    int32_t depthOverride;
    
    int32_t soulRescueValue;
    
    int32_t with2_a, with2_b;       
    int32_t with21_a;               
    int32_t with3_a, with3_b;       
    int32_t with31_a, with31_b;     
    bool hasPd1591AtCon2;           
} ActPattern;


static struct {
    int32_t gTyper, gSoulRescue, gLanguage;
    int32_t globalMsgVarId;
    int32_t actInitialized;
} actSharedCache = { .actInitialized = 0 };

static void initActSharedCache(VMContext* ctx, DataWin* dw) {
    if (actSharedCache.actInitialized) return;
    actSharedCache.gTyper      = findGlobalVarId(ctx, "typer");
    actSharedCache.gSoulRescue = findGlobalVarId(ctx, "soul_rescue");
    actSharedCache.gLanguage   = findGlobalVarId(ctx, "language");
    actSharedCache.globalMsgVarId = findGlobalVarId(ctx, "msg");
    actSharedCache.actInitialized = 1;
    (void)dw;
}


static void actWithEventUser(Runner* runner, int32_t objectIdx, int32_t userSubtype) {
    if (objectIdx <= 0) return;
    for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
        Instance* it = runner->instances[i];
        if (it && it->objectIndex == objectIdx && it->active) {
            Runner_executeEvent(runner, it, EVENT_OTHER, OTHER_USER0 + userSubtype);
        }
    }
}


static void actWithSelfVarSet(Runner* runner, int32_t objectIdx, int32_t varId, GMLReal val) {
    if (objectIdx <= 0 || varId < 0) return;
    for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
        Instance* it = runner->instances[i];
        if (it && it->objectIndex == objectIdx && it->active) {
            Instance_setSelfVar(it, varId, RValue_makeReal(val));
        }
    }
}

static void actRunPattern(VMContext* ctx, Runner* runner, Instance* inst, const ActPattern* p) {
    if (runner->renderer == NULL) return;

    
    
    int32_t conVar = p->con;
    if (p->conCandidateCount > 0) {
        conVar = resolveSelfVarIdForInst(inst, p->conCandidates, p->conCandidateCount);
    }
    if (conVar < 0) return;

    Renderer* r = runner->renderer;

    
    Renderer_drawSpriteExt(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, 1.0f, 1.0f, inst->imageAngle,
                           0xFFFFFFu, inst->imageAlpha);

    GMLReal con = selfReal(inst, conVar);

    
    if (con > 0.0 && con < 3.0) {
        if (p->depthOverride != 0) inst->depth = p->depthOverride;
        inst->imageAlpha -= p->alphaDec;

        r->vtable->drawRectangle(r, p->ox1, p->oy1, p->ox2, p->oy2, 0xFFFFFFu, 1.0f, false);
        r->vtable->drawRectangle(r, p->ix1, p->iy1, p->ix2, p->iy2, 0x000000u, 1.0f, false);

        
        bool isJa = false;
        if (actSharedCache.gLanguage >= 0) {
            RValue lv = ctx->globalVars[actSharedCache.gLanguage];
            if (lv.type == RVALUE_STRING && lv.string && strcmp(lv.string, "ja") == 0) isJa = true;
        }
        r->drawFont = isJa ? 14 : 2;

        
        if (actSharedCache.gTyper >= 0)
            globalSet(ctx, actSharedCache.gTyper, RValue_makeReal(113.0));
        BuiltinFunc gtFn = VMBuiltins_find("scr_gettext");
        if (gtFn && actSharedCache.globalMsgVarId >= 0) {
            for (int32_t slot = 0; slot < 2; slot++) {
                RValue arg = RValue_makeString(slot == 0 ? p->msg0Key : p->msg1Key);
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = gtFn(ctx, &arg, 1);
                ctx->currentInstance = saved;

                
                
                
                RValue nv;
                if (res.type == RVALUE_STRING && res.string) {
                    nv = RValue_makeOwnedString(safeStrdup(res.string));
                } else {
                    nv = res;
                    
                    res.type = RVALUE_UNDEFINED;
                    res.string = NULL;
                    res.ownsString = false;
                }
                int64_t k = ((int64_t)actSharedCache.globalMsgVarId << 32) | (uint32_t)slot;
                ptrdiff_t idx = hmgeti(ctx->globalArrayMap, k);
                if (idx >= 0) { RValue_free(&ctx->globalArrayMap[idx].value); ctx->globalArrayMap[idx].value = nv; }
                else          { ArrayMapEntry e = { .key = k, .value = nv }; hmputs(ctx->globalArrayMap, e); }
                RValue_free(&res);  
            }
        }

        
        if (findInstanceByObject(runner, 1604) == NULL) {
            Runner_createInstance(runner, p->writerX, p->writerY, 1604);
        }
    }

    if (con == 2.0) {
        if (p->type >= 0) Instance_setSelfVar(inst, p->type, RValue_makeReal(1.0));
        actWithEventUser(runner, p->with2_a, 5);
        actWithEventUser(runner, p->with2_b, 5);
        if (p->hasPd1591AtCon2) {
            
            int32_t pdVar = findSelfVarId(ctx->dataWin, "pd");
            actWithSelfVarSet(runner, 1591, pdVar, 1.0);
        }
        Instance_setSelfVar(inst, conVar, RValue_makeReal(2.1));
        inst->alarm[4] = 50;
    } else if (con == 2.1 && p->with21_a > 0) {
        actWithEventUser(runner, p->with21_a, 5);
    } else if (con == 3.0) {
        GMLReal cur = (actSharedCache.gSoulRescue >= 0)
                       ? globalReal(ctx, actSharedCache.gSoulRescue) : 0.0;
        if ((int32_t)cur != p->soulRescueValue) {
            if (actSharedCache.gSoulRescue >= 0)
                globalSet(ctx, actSharedCache.gSoulRescue, RValue_makeReal((GMLReal)p->soulRescueValue));
            
            BuiltinFunc iw = VMBuiltins_find("ini_write_real");
            if (iw) {
                RValue args[3] = {
                    RValue_makeString("FFFFF"),
                    RValue_makeString("P"),
                    RValue_makeReal((GMLReal)(p->soulRescueValue + 1))
                };
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = iw(ctx, args, 3);
                ctx->currentInstance = saved;
                RValue_free(&res);
            }
            
        }
        
        actWithEventUser(runner, p->with3_a, 4);
        actWithEventUser(runner, p->with3_b, 4);
    } else if (con == 3.1) {
        
        for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
            Instance* it = runner->instances[i];
            if (it && it->objectIndex == 1604 && it->active)
                Runner_destroyInstance(runner, it);
        }
        
        BuiltinFunc sp = VMBuiltins_find("snd_play");
        if (sp) {
            RValue arg = RValue_makeReal(155.0);
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = sp(ctx, &arg, 1);
            ctx->currentInstance = saved;
            RValue_free(&res);
        }
        
        Runner_createInstance(runner, 0.0, 0.0, 1608);
        
        
        int32_t soultimerVar = findSelfVarId(ctx->dataWin, "soultimer");
        int32_t soulmaxVar   = findSelfVarId(ctx->dataWin, "soulmax");
        if (soultimerVar >= 0 && soulmaxVar >= 0) {
            for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
                Instance* it = runner->instances[i];
                if (it && it->objectIndex == 1590 && it->active) {
                    GMLReal sm = selfReal(it, soulmaxVar);
                    Instance_setSelfVar(it, soultimerVar, RValue_makeReal(sm - 150.0));
                }
            }
        }
        actWithEventUser(runner, p->with31_a, 4);
        actWithEventUser(runner, p->with31_b, 4);
        Instance_setSelfVar(inst, conVar, RValue_makeReal(3.0));
    }
}




static struct {
    int32_t conVarIds[ACT_MAX_CON_VARIDS]; int32_t conCount;
    int32_t type; bool ready;
} knifeActCache = { .ready = false };
static void initKnifeActCache(DataWin* dw) {
    knifeActCache.conCount = findAllSelfVarIds(dw, "con", knifeActCache.conVarIds, ACT_MAX_CON_VARIDS);
    knifeActCache.type = findSelfVarId(dw, "type");
    knifeActCache.ready = (knifeActCache.conCount > 0);
}
static void native_knifeAct_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!knifeActCache.ready) return;
    ActPattern p = {
        .con = -1, .type = knifeActCache.type,
        .conCandidates = knifeActCache.conVarIds, .conCandidateCount = knifeActCache.conCount,
        .ox1 = 106, .oy1 = 146, .ox2 = 534, .oy2 = 204,
        .ix1 = 110, .iy1 = 150, .ix2 = 530, .iy2 = 200,
        .writerX = 110, .writerY = 140,
        .msg0Key = "obj_6knife_act_284", .msg1Key = "obj_6knife_act_285",
        .alphaDec = 0.02f, .depthOverride = 0, .soulRescueValue = 1,
        .with2_a = 1631, .with2_b = 0, .with21_a = 0,
        .with3_a = 0, .with3_b = 0,
        .with31_a = 1631, .with31_b = 0,
        .hasPd1591AtCon2 = true
    };
    actRunPattern(ctx, runner, inst, &p);
}




static struct {
    int32_t conVarIds[ACT_MAX_CON_VARIDS]; int32_t conCount;
    int32_t type; bool ready;
} gloveActCache = { .ready = false };
static void initGloveActCache(DataWin* dw) {
    gloveActCache.conCount = findAllSelfVarIds(dw, "con", gloveActCache.conVarIds, ACT_MAX_CON_VARIDS);
    gloveActCache.type = findSelfVarId(dw, "type");
    gloveActCache.ready = (gloveActCache.conCount > 0);
}
static void native_gloveAct_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!gloveActCache.ready) return;
    
    int32_t conVar = resolveSelfVarIdForInst(inst, gloveActCache.conVarIds, gloveActCache.conCount);
    if (runner->renderer != NULL && conVar >= 0 &&
        selfReal(inst, conVar) > 0.0 && selfReal(inst, conVar) < 3.0) {
        actWithEventUser(runner, 1623, 5);
    }
    ActPattern p = {
        .con = -1, .type = gloveActCache.type,
        .conCandidates = gloveActCache.conVarIds, .conCandidateCount = gloveActCache.conCount,
        .ox1 = 106, .oy1 = 146, .ox2 = 534, .oy2 = 204,
        .ix1 = 110, .iy1 = 150, .ix2 = 530, .iy2 = 200,
        .writerX = 110, .writerY = 140,
        .msg0Key = "obj_6glove_act_148", .msg1Key = "obj_6glove_act_149",
        .alphaDec = 0.02f, .depthOverride = 0, .soulRescueValue = 2,
        .with2_a = 1621, .with2_b = 0, .with21_a = 1621,
        .with3_a = 1621, .with3_b = 0,
        .with31_a = 1621, .with31_b = 0,
        .hasPd1591AtCon2 = true
    };
    actRunPattern(ctx, runner, inst, &p);
}




static struct {
    int32_t conVarIds[ACT_MAX_CON_VARIDS]; int32_t conCount;
    int32_t type; bool ready;
} shoeActCache = { .ready = false };
static void initShoeActCache(DataWin* dw) {
    shoeActCache.conCount = findAllSelfVarIds(dw, "con", shoeActCache.conVarIds, ACT_MAX_CON_VARIDS);
    shoeActCache.type = findSelfVarId(dw, "type");
    shoeActCache.ready = (shoeActCache.conCount > 0);
}
static void native_shoeAct_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!shoeActCache.ready) return;
    int32_t conVar = resolveSelfVarIdForInst(inst, shoeActCache.conVarIds, shoeActCache.conCount);
    ActPattern p = {
        .con = -1, .type = shoeActCache.type,
        .conCandidates = shoeActCache.conVarIds, .conCandidateCount = shoeActCache.conCount,
        .ox1 = 106, .oy1 = 146, .ox2 = 534, .oy2 = 204,
        .ix1 = 110, .iy1 = 150, .ix2 = 530, .iy2 = 200,
        .writerX = 110, .writerY = 140,
        .msg0Key = "obj_6shoe_act_184", .msg1Key = "obj_6shoe_act_185",
        .alphaDec = 0.02f, .depthOverride = -31, .soulRescueValue = 3,
        .with2_a = 1618, .with2_b = 1620, .with21_a = 0,  
        .with3_a = 1621, .with3_b = 0,  
        .with31_a = 1618, .with31_b = 1620,
        .hasPd1591AtCon2 = true
    };
    
    if (runner->renderer != NULL && conVar >= 0 && selfReal(inst, conVar) == 2.1) {
        actWithEventUser(runner, 1618, 5);
        actWithEventUser(runner, 1620, 5);
    }
    actRunPattern(ctx, runner, inst, &p);
}




static struct {
    int32_t conVarIds[ACT_MAX_CON_VARIDS]; int32_t conCount;
    int32_t type; bool ready;
} panActCache = { .ready = false };
static void initPanActCache(DataWin* dw) {
    panActCache.conCount = findAllSelfVarIds(dw, "con", panActCache.conVarIds, ACT_MAX_CON_VARIDS);
    panActCache.type = findSelfVarId(dw, "type");
    panActCache.ready = (panActCache.conCount > 0);
}
static void native_panAct_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!panActCache.ready) return;
    ActPattern p = {
        .con = -1, .type = panActCache.type,
        .conCandidates = panActCache.conVarIds, .conCandidateCount = panActCache.conCount,
        .ox1 = 106, .oy1 = 146, .ox2 = 534, .oy2 = 204,
        .ix1 = 110, .iy1 = 150, .ix2 = 530, .iy2 = 200,
        .writerX = 110, .writerY = 140,
        .msg0Key = "obj_6pan_act_163", .msg1Key = "obj_6pan_act_164",
        .alphaDec = 0.05f, .depthOverride = -14, .soulRescueValue = 5,
        .with2_a = 1624, .with2_b = 1625, .with21_a = 1624,
        .with3_a = 1624, .with3_b = 0,
        .with31_a = 1624, .with31_b = 1625,
        .hasPd1591AtCon2 = true
    };
    actRunPattern(ctx, runner, inst, &p);
}




static struct {
    int32_t conVarIds[ACT_MAX_CON_VARIDS]; int32_t conCount;
    int32_t type; bool ready;
} gunActCache = { .ready = false };
static void initGunActCache(DataWin* dw) {
    gunActCache.conCount = findAllSelfVarIds(dw, "con", gunActCache.conVarIds, ACT_MAX_CON_VARIDS);
    gunActCache.type = findSelfVarId(dw, "type");
    gunActCache.ready = (gunActCache.conCount > 0);
}
static void native_gunAct_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!gunActCache.ready) return;
    ActPattern p = {
        .con = -1, .type = gunActCache.type,
        .conCandidates = gunActCache.conVarIds, .conCandidateCount = gunActCache.conCount,
        .ox1 = 106, .oy1 = 346, .ox2 = 534, .oy2 = 404,
        .ix1 = 110, .iy1 = 350, .ix2 = 530, .iy2 = 400,
        .writerX = 110, .writerY = 340,
        .msg0Key = "obj_6gun_act_149", .msg1Key = "obj_6gun_act_150",
        .alphaDec = 0.02f, .depthOverride = 0, .soulRescueValue = 6,
        .with2_a = 1615, .with2_b = 0, .with21_a = 0,
        .with3_a = 1613, .with3_b = 0,
        .with31_a = 1615, .with31_b = 0,
        .hasPd1591AtCon2 = true
    };
    actRunPattern(ctx, runner, inst, &p);
}




static struct {
    int32_t conVarIds[ACT_MAX_CON_VARIDS]; int32_t conCount;
    int32_t type, booky, booky2;
    bool ready;
} bookMasterCache = { .ready = false };
static void initBookMasterCache(DataWin* dw) {
    bookMasterCache.conCount = findAllSelfVarIds(dw, "con", bookMasterCache.conVarIds, ACT_MAX_CON_VARIDS);
    bookMasterCache.type  = findSelfVarId(dw, "type");
    bookMasterCache.booky = findSelfVarId(dw, "booky");
    bookMasterCache.booky2 = findSelfVarId(dw, "booky2");
    bookMasterCache.ready = (bookMasterCache.conCount > 0 && bookMasterCache.booky >= 0);
}
static void native_bookMaster_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!bookMasterCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    GMLReal booky  = selfReal(inst, bookMasterCache.booky);
    GMLReal booky2 = selfReal(inst, bookMasterCache.booky2);
    int32_t imgIdx = (int32_t)inst->imageIndex;
    for (int32_t i = 0; i < 6; i++) {
        Renderer_drawSpriteExt(r, 2329, imgIdx,
                               100.0f, -170.0f + (float)(170 * i) + (float)booky,
                               2.0f, 2.0f, 0.0f, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 2329, imgIdx,
                               540.0f, (float)(170 * i) + (float)booky2,
                               -2.0f, -2.0f, 0.0f, 0xFFFFFFu, 1.0f);
    }
    booky += 4.0;  booky2 -= 4.0;
    if (booky > 170.0)   booky -= 170.0;
    if (booky2 < -170.0) booky2 += 170.0;
    Instance_setSelfVar(inst, bookMasterCache.booky,  RValue_makeReal(booky));
    Instance_setSelfVar(inst, bookMasterCache.booky2, RValue_makeReal(booky2));

    
    
    ActPattern p = {
        .con = -1, .type = bookMasterCache.type,
        .conCandidates = bookMasterCache.conVarIds, .conCandidateCount = bookMasterCache.conCount,
        .ox1 = 4, .oy1 = 4, .ox2 = 140, .oy2 = 230,
        .ix1 = 8, .iy1 = 8, .ix2 = 136, .iy2 = 226,
        .writerX = 14, .writerY = 4,
        .msg0Key = "obj_6book_master_384", .msg1Key = "obj_6book_master_385",
        .alphaDec = 0.02f, .depthOverride = 0, .soulRescueValue = 4,
        .with2_a = 1628, .with2_b = 0, .with21_a = 0,
        .with3_a = 0, .with3_b = 0,
        .with31_a = 1628, .with31_b = 0,
        .hasPd1591AtCon2 = true
    };
    
    
    int32_t conVar = resolveSelfVarIdForInst(inst, bookMasterCache.conVarIds, bookMasterCache.conCount);
    if (conVar < 0) return;
    GMLReal con = selfReal(inst, conVar);
    if (con > 0.0 && con < 3.0) {
        inst->imageAlpha -= p.alphaDec;
        r->vtable->drawRectangle(r, p.ox1, p.oy1, p.ox2, p.oy2, 0xFFFFFFu, 1.0f, false);
        r->vtable->drawRectangle(r, p.ix1, p.iy1, p.ix2, p.iy2, 0x000000u, 1.0f, false);
        bool isJa = false;
        if (actSharedCache.gLanguage >= 0) {
            RValue lv = ctx->globalVars[actSharedCache.gLanguage];
            if (lv.type == RVALUE_STRING && lv.string && strcmp(lv.string, "ja") == 0) isJa = true;
        }
        r->drawFont = isJa ? 14 : 2;
        if (actSharedCache.gTyper >= 0)
            globalSet(ctx, actSharedCache.gTyper, RValue_makeReal(113.0));
        BuiltinFunc gtFn = VMBuiltins_find("scr_gettext");
        if (gtFn && actSharedCache.globalMsgVarId >= 0) {
            for (int32_t slot = 0; slot < 2; slot++) {
                RValue arg = RValue_makeString(slot == 0 ? p.msg0Key : p.msg1Key);
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = gtFn(ctx, &arg, 1);
                ctx->currentInstance = saved;
                int64_t k = ((int64_t)actSharedCache.globalMsgVarId << 32) | (uint32_t)slot;
                RValue nv = res;
                if (nv.type == RVALUE_STRING && nv.string)
                    nv = RValue_makeOwnedString(safeStrdup(nv.string));
                ptrdiff_t idx = hmgeti(ctx->globalArrayMap, k);
                if (idx >= 0) { RValue_free(&ctx->globalArrayMap[idx].value); ctx->globalArrayMap[idx].value = nv; }
                else          { ArrayMapEntry e = { .key = k, .value = nv }; hmputs(ctx->globalArrayMap, e); }
                RValue_free(&res);
            }
        }
        if (findInstanceByObject(runner, 1604) == NULL) {
            Runner_createInstance(runner, p.writerX, p.writerY, 1604);
        }
    }
    if (con == 2.0) {
        if (p.type >= 0) Instance_setSelfVar(inst, p.type, RValue_makeReal(1.0));
        int32_t pdVar = findSelfVarId(ctx->dataWin, "pd");
        actWithSelfVarSet(runner, 1591, pdVar, 1.0);
        actWithEventUser(runner, 1628, 5);
        Instance_setSelfVar(inst, conVar, RValue_makeReal(2.1));
        inst->alarm[4] = 50;
    } else if (con == 3.0) {
        GMLReal cur = (actSharedCache.gSoulRescue >= 0)
                       ? globalReal(ctx, actSharedCache.gSoulRescue) : 0.0;
        if ((int32_t)cur != p.soulRescueValue) {
            globalSet(ctx, actSharedCache.gSoulRescue, RValue_makeReal((GMLReal)p.soulRescueValue));
            BuiltinFunc iw = VMBuiltins_find("ini_write_real");
            if (iw) {
                RValue args[3] = {
                    RValue_makeString("FFFFF"), RValue_makeString("P"),
                    RValue_makeReal((GMLReal)(p.soulRescueValue + 1))
                };
                Instance* saved = (Instance*)ctx->currentInstance;
                ctx->currentInstance = inst;
                RValue res = iw(ctx, args, 3);
                ctx->currentInstance = saved;
                RValue_free(&res);
            }
        }
    } else if (con == 3.1) {
        for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
            Instance* it = runner->instances[i];
            if (it && it->objectIndex == 1604 && it->active)
                Runner_destroyInstance(runner, it);
        }
        BuiltinFunc sp = VMBuiltins_find("snd_play");
        if (sp) {
            RValue arg = RValue_makeReal(155.0);
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = sp(ctx, &arg, 1);
            ctx->currentInstance = saved;
            RValue_free(&res);
        }
        Runner_createInstance(runner, 0.0, 0.0, 1608);
        int32_t soultimerVar = findSelfVarId(ctx->dataWin, "soultimer");
        int32_t soulmaxVar   = findSelfVarId(ctx->dataWin, "soulmax");
        if (soultimerVar >= 0 && soulmaxVar >= 0) {
            for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
                Instance* it = runner->instances[i];
                if (it && it->objectIndex == 1590 && it->active) {
                    GMLReal sm = selfReal(it, soulmaxVar);
                    Instance_setSelfVar(it, soultimerVar, RValue_makeReal(sm - 150.0));
                }
            }
        }
        actWithEventUser(runner, 1628, 4);
        Instance_setSelfVar(inst, conVar, RValue_makeReal(3.0));
    }

    
    Instance* heart = findInstanceByObject(runner, findObjectIndex(ctx->dataWin, "obj_vsflowey_heart"));
    if (heart) {
        if (heart->x < 245.0f) heart->x = 245.0f;
        if (heart->x > 382.0f) heart->x = 382.0f;
        if (heart->y < 138.0f) heart->y = 138.0f;
    }
}

#endif  




static struct {
    int32_t siner, rot, counter, ss, num, spec;
    int32_t xox, yoy;
    bool ready;
} panCache = { .ready = false };
static void initPanCache(DataWin* dw) {
    panCache.siner   = findSelfVarId(dw, "siner");
    panCache.rot     = findSelfVarId(dw, "rot");
    panCache.counter = findSelfVarId(dw, "counter");
    panCache.ss      = findSelfVarId(dw, "ss");
    panCache.num     = findSelfVarId(dw, "num");
    panCache.spec    = findSelfVarId(dw, "spec");
    panCache.xox     = findSelfVarId(dw, "xox");
    panCache.yoy     = findSelfVarId(dw, "yoy");
    panCache.ready = (panCache.rot >= 0 && panCache.counter >= 0 && panCache.ss >= 0);
}
static void native_pan_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!panCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, panCache.siner) + 1.0;
    GMLReal rot = selfReal(inst, panCache.rot);
    GMLReal counter = selfReal(inst, panCache.counter);
    int32_t ss = selfInt(inst, panCache.ss);
    int32_t num = selfInt(inst, panCache.num);
    int32_t spec = (panCache.spec >= 0) ? selfInt(inst, panCache.spec) : 0;

    
    float radA = (float)((rot + 180.0) * (M_PI / 180.0));
    float xox = 220.0f * GMLReal_cos(radA);
    float yoy = -220.0f * GMLReal_sin(radA);

    
    BuiltinFunc dt = VMBuiltins_find("draw_triangle");
    if (dt) {
        uint32_t savedColor = r->drawColor;
        r->drawColor = 0x000000u;
        RValue args[7] = {
            RValue_makeReal(inst->x), RValue_makeReal(inst->y),
            RValue_makeReal(inst->x + xox), RValue_makeReal(inst->y + yoy),
            RValue_makeReal(-20.0 + inst->x + xox / 2.0), RValue_makeReal(inst->y + 80.0),
            RValue_makeReal(0.0)  
        };
        Instance* saved = (Instance*)ctx->currentInstance;
        ctx->currentInstance = inst;
        RValue res = dt(ctx, args, 7);
        ctx->currentInstance = saved;
        RValue_free(&res);
        r->drawColor = savedColor;
    }

    if (ss == 1) {
        inst->x += (float)(GMLReal_sin(siner / 3.0) * 5.0);
        inst->y += (float)(GMLReal_cos(siner / 2.0) * 2.0);
    }

    int32_t imgIdx = (int32_t)inst->imageIndex;
    if (imgIdx == 0) {
        Renderer_drawSpriteExt(r, inst->spriteIndex, 0, inst->x, inst->y,
                               inst->imageXscale, inst->imageYscale, (float)rot,
                               0xFFFFFFu, inst->imageAlpha);
    } else if (imgIdx == 1) {
        Renderer_drawSpriteExt(r, inst->spriteIndex, 1, inst->x, inst->y,
                               inst->imageXscale, inst->imageYscale, (float)(rot + 40.0),
                               0xFFFFFFu, inst->imageAlpha);
    }

    counter += 1.0;
    if (counter > 57.0 && counter < 60.0) rot += 2.0;
    if (counter > 60.0 && counter < 62.0) {
        rot -= 8.0;
        if (spec == 1) num += 1;
        if (num != 12) {
            float a2 = (float)((rot + 180.0) * (M_PI / 180.0));
            float xox2 = 150.0f * GMLReal_cos(a2);
            
            
            float yoy2 = 70.0f * GMLReal_cos(a2);
            int32_t panparentVar = findSelfVarId(ctx->dataWin, "panparent");
            int32_t gravVar = findSelfVarId(ctx->dataWin, "gravity");
            (void)gravVar; 

            Instance* fr;
            fr = Runner_createInstance(runner, inst->x + xox2, inst->y + yoy2, 1624);
            if (fr) fr->gravity += 0.1f + (float)((double)rand()/(double)RAND_MAX * 0.08);
            Runner_createInstance(runner, inst->x + xox2, inst->y + yoy2, 1624);
            fr = Runner_createInstance(runner, inst->x + xox2, inst->y + yoy2, 1624);
            if (fr) fr->gravity += 0.07f + (float)((double)rand()/(double)RAND_MAX * 0.06);
            fr = Runner_createInstance(runner, inst->x + xox2, inst->y + yoy2, 1624);
            if (fr) {
                fr->gravity += 0.05f + (float)((double)rand()/(double)RAND_MAX * 0.04);
                if (panparentVar >= 0)
                    Instance_setSelfVar(fr, panparentVar, RValue_makeReal((GMLReal)inst->instanceId));
            }
        } else {
            Runner_createInstance(runner, inst->x - 140.0f, inst->y - 10.0f, 1626);
        }
    }
    if (counter >= 63.0 && counter < 67.0) rot -= 3.0;
    if (counter >= 63.0 && counter < 72.0) ss = 0;
    if (counter > 70.0 && counter < 72.0) rot += 6.0;
    if (counter >= 72.0) {
        inst->imageIndex = 0.0f;
        rot += 5.0;
        if (rot > 3.0) {
            ss = 1; rot = 0.0; counter = 50.0;
            if (num == 12) counter = -20.0;
        }
    }

    Instance_setSelfVar(inst, panCache.siner,   RValue_makeReal(siner));
    Instance_setSelfVar(inst, panCache.rot,     RValue_makeReal(rot));
    Instance_setSelfVar(inst, panCache.counter, RValue_makeReal(counter));
    Instance_setSelfVar(inst, panCache.ss,      RValue_makeReal((GMLReal)ss));
    if (panCache.num >= 0) Instance_setSelfVar(inst, panCache.num, RValue_makeReal((GMLReal)num));
    if (panCache.xox >= 0) Instance_setSelfVar(inst, panCache.xox, RValue_makeReal((GMLReal)xox));
    if (panCache.yoy >= 0) Instance_setSelfVar(inst, panCache.yoy, RValue_makeReal((GMLReal)yoy));
}






static struct {
    int32_t dmg, apparenthp, stretchfactor, stretchwidth, numnum;
    int32_t thisnum, place, numadd, thisnum2;
    int32_t gFloweymaxhp;
    bool ready;
} floweyDmgCache = { .ready = false };

static void initFloweyDmgCache(VMContext* ctx, DataWin* dw) {
    floweyDmgCache.dmg           = findSelfVarId(dw, "dmg");
    floweyDmgCache.apparenthp    = findSelfVarId(dw, "apparenthp");
    floweyDmgCache.stretchfactor = findSelfVarId(dw, "stretchfactor");
    floweyDmgCache.stretchwidth  = findSelfVarId(dw, "stretchwidth");
    floweyDmgCache.numnum        = findSelfVarId(dw, "numnum");
    floweyDmgCache.thisnum       = findSelfVarId(dw, "thisnum");
    floweyDmgCache.place         = findSelfVarId(dw, "place");
    floweyDmgCache.numadd        = findSelfVarId(dw, "numadd");
    floweyDmgCache.thisnum2      = findSelfVarId(dw, "thisnum2");
    floweyDmgCache.gFloweymaxhp  = findGlobalVarId(ctx, "floweymaxhp");
    floweyDmgCache.ready = (floweyDmgCache.dmg >= 0 && floweyDmgCache.apparenthp >= 0 &&
                            floweyDmgCache.stretchfactor >= 0 && floweyDmgCache.gFloweymaxhp >= 0);
}

static void native_floweyDmgWriter_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!floweyDmgCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal dmg = selfReal(inst, floweyDmgCache.dmg);
    GMLReal apparenthp = selfReal(inst, floweyDmgCache.apparenthp);
    GMLReal stretchfactor = selfReal(inst, floweyDmgCache.stretchfactor);
    GMLReal stretchwidth = (floweyDmgCache.stretchwidth >= 0) ? selfReal(inst, floweyDmgCache.stretchwidth) : 0.0;
    GMLReal floweymaxhp = globalReal(ctx, floweyDmgCache.gFloweymaxhp);

    
    GMLReal thisnum;
    int32_t place = 0;
    GMLReal numadd = 10.0;
    if (dmg >= 0) {
        thisnum = dmg;
        if (thisnum >= numadd) {
            while (thisnum >= numadd) {
                place += 1;
                numadd *= 10.0;
            }
        }
    } else {
        thisnum = 0.0;
        place = 0;
    }

    
    int32_t digits[8] = {0};
    int32_t cappedPlace = (place < 7) ? place : 7;
    GMLReal thisnum2 = thisnum;
    for (int32_t i = cappedPlace; i >= 0; i--) {
        GMLReal pow10 = 1.0;
        for (int32_t k = 0; k < i; k++) pow10 *= 10.0;
        int32_t digit = (int32_t)(thisnum2 / pow10);  
        digits[i] = digit;
        thisnum2 -= (GMLReal)digit * pow10;
        
        if (floweyDmgCache.numnum >= 0)
            selfArraySet(inst, floweyDmgCache.numnum, i, RValue_makeReal((GMLReal)digit));
    }

    
    if (floweyDmgCache.thisnum  >= 0) Instance_setSelfVar(inst, floweyDmgCache.thisnum,  RValue_makeReal(thisnum));
    if (floweyDmgCache.thisnum2 >= 0) Instance_setSelfVar(inst, floweyDmgCache.thisnum2, RValue_makeReal(thisnum2));
    if (floweyDmgCache.place    >= 0) Instance_setSelfVar(inst, floweyDmgCache.place,    RValue_makeReal((GMLReal)place));
    if (floweyDmgCache.numadd   >= 0) Instance_setSelfVar(inst, floweyDmgCache.numadd,   RValue_makeReal(numadd));

    
    
    
    
    float barRight = inst->x + (float)floweymaxhp * (float)stretchfactor;
    r->vtable->drawRectangle(r, inst->x - 1.0f, inst->ystart + 7.0f,
                             barRight + 1.0f, inst->ystart + 28.0f,
                             0x000000u, 1.0f, false);
    r->vtable->drawRectangle(r, inst->x, inst->ystart + 8.0f,
                             barRight, inst->ystart + 28.0f,
                             0x404040u, 1.0f, false);
    if (apparenthp > 0.0) {
        float fillRight = inst->x + (float)apparenthp * (float)stretchfactor;
        
        r->vtable->drawRectangle(r, inst->x, inst->ystart + 8.0f,
                                 fillRight, inst->ystart + 28.0f,
                                 0x00FF00u, 1.0f, false);
    }

    
    
    for (int32_t i = cappedPlace; i >= 0; i--) {
        float sx = (((inst->x - 20.0f) + (float)stretchwidth / 2.0f) - (float)(i * 32)) + (float)(place * 16);
        Renderer_drawSpriteExt(r, 42, digits[i], sx, inst->y - 28.0f,
                               1.0f, 1.0f, 0.0f, 0x0000FFu, 1.0f);
    }

    
    if (inst->y > inst->ystart) {
        inst->y = inst->ystart;
        inst->vspeed = 0.0f;
        inst->gravity = 0.0f;
    }
}






static struct {
    int32_t dr, fog_r, fog_alpha, s;
    int32_t gPlot;
    int32_t objMainchara;
    bool ready;
} fogmakerCache = { .ready = false };

static void initFogmakerCache(VMContext* ctx, DataWin* dw) {
    fogmakerCache.dr        = findSelfVarId(dw, "dr");
    fogmakerCache.fog_r     = findSelfVarId(dw, "fog_r");
    fogmakerCache.fog_alpha = findSelfVarId(dw, "fog_alpha");
    fogmakerCache.s         = findSelfVarId(dw, "s");
    fogmakerCache.gPlot     = findGlobalVarId(ctx, "plot");
    fogmakerCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    fogmakerCache.ready = (fogmakerCache.fog_r >= 0 && fogmakerCache.fog_alpha >= 0 &&
                           fogmakerCache.dr >= 0 && fogmakerCache.s >= 0);
}

static void native_fogmaker_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!fogmakerCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    GMLReal dr = selfReal(inst, fogmakerCache.dr) + 1.0;
    GMLReal fog_r = selfReal(inst, fogmakerCache.fog_r);
    if (dr > 3.0) {
        fog_r += 1.0;
        dr = 0.0;
    }
    Instance_setSelfVar(inst, fogmakerCache.dr, RValue_makeReal(dr));

    
    int32_t sFlag = selfInt(inst, fogmakerCache.s);
    GMLReal fog_alpha = selfReal(inst, fogmakerCache.fog_alpha);
    if (sFlag == 0) {
        Instance* mainchara = (fogmakerCache.objMainchara >= 0)
                              ? findInstanceByObject(runner, fogmakerCache.objMainchara) : NULL;
        if (mainchara) fog_alpha = (GMLReal)mainchara->x / 440.0;

        
        GMLReal plot = (fogmakerCache.gPlot >= 0) ? globalReal(ctx, fogmakerCache.gPlot) : 0.0;
        if (fog_alpha > 1.0 && plot > 99.0) {
            fog_alpha = 1.0 + (1.0 - fog_alpha);
        }
    }
    Instance_setSelfVar(inst, fogmakerCache.fog_alpha, RValue_makeReal(fog_alpha));

    
    
    int32_t tpagIndex = Renderer_resolveTPAGIndex(ctx->dataWin, 2032, 0);
    if (tpagIndex < 0) {
        
    } else {
        float alpha = (float)fog_alpha;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        int32_t src_x = (int32_t)fog_r;
        for (int32_t j = 0; j < 3; j++) {
            float dst_y = (float)(j * 80);
            for (int32_t i = 0; i < 13; i++) {
                r->vtable->drawSpritePart(r, tpagIndex, src_x, 0, 80, 80,
                                          (float)(i * 80), dst_y,
                                          1.0f, 1.0f, 0xFFFFFFu, alpha);
            }
        }
    }

    
    if (fog_r >= 80.0) fog_r -= 80.0;
    Instance_setSelfVar(inst, fogmakerCache.fog_r, RValue_makeReal(fog_r));
}






static struct {
    int32_t blue, drawn;
    int32_t gIdealborder, gInvc;
    int32_t objHeart;
    bool ready;
} topboneCache = { .ready = false };

static void initTopboneCache(VMContext* ctx, DataWin* dw) {
    topboneCache.blue  = findSelfVarId(dw, "blue");
    topboneCache.drawn = findSelfVarId(dw, "drawn");
    topboneCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    topboneCache.gInvc        = findGlobalVarId(ctx, "invc");
    topboneCache.objHeart = findObjectIndex(dw, "obj_heart");
    topboneCache.ready = (topboneCache.blue >= 0 && topboneCache.gIdealborder >= 0);
}

static inline GMLReal topboneGetBorder(VMContext* ctx, int32_t idx) {
    if (topboneCache.gIdealborder < 0) return 0.0;
    int64_t k = ((int64_t)topboneCache.gIdealborder << 32) | (uint32_t)idx;
    ptrdiff_t p = hmgeti(ctx->globalArrayMap, k);
    return (p >= 0) ? RValue_toReal(ctx->globalArrayMap[p].value) : 0.0;
}

static void native_topbone_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!topboneCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    float spriteW = 0.0f, spriteH = 0.0f;
    if (inst->spriteIndex >= 0 && (uint32_t)inst->spriteIndex < ctx->dataWin->sprt.count) {
        spriteW = (float)ctx->dataWin->sprt.sprites[inst->spriteIndex].width * inst->imageXscale;
        spriteH = (float)ctx->dataWin->sprt.sprites[inst->spriteIndex].height * inst->imageYscale;
    }

    
    float ibL = (float)topboneGetBorder(ctx, 0);
    float ibR = (float)topboneGetBorder(ctx, 1);
    float ibT = (float)topboneGetBorder(ctx, 2);
    float ibB = (float)topboneGetBorder(ctx, 3);

    float l = 0.0f, t = 0.0f, w = spriteW, h = spriteH;
    float ll = (ibL - inst->x) + 1.0f;
    float tt = (ibT - inst->y) + 1.0f;
    float ww = (inst->x + w) - ibR - 1.0f;
    float hh = (inst->y + h) - ibB - 1.0f;
    if (ll > 0.0f) l += ll;
    if (tt > 0.0f) t += tt;
    if (ww > 0.0f) w -= ww;
    if (hh > 0.0f) h -= hh;

    
    int32_t lR = (int32_t)floorf(l + 0.5f);
    int32_t tR = (int32_t)floorf(t + 0.5f);
    int32_t wR = (int32_t)floorf(w + 0.5f);
    int32_t hR = (int32_t)floorf(h + 0.5f);

    int32_t blue = selfInt(inst, topboneCache.blue);

    if (wR > 0 && hR > 0 && lR < wR && tR < hR) {
        
        if (blue == 1) inst->imageIndex = 1.0f;
        int32_t subimg = (int32_t)inst->imageIndex;

        
        int32_t tp123 = Renderer_resolveTPAGIndex(ctx->dataWin, 123, subimg);
        if (tp123 >= 0) {
            r->vtable->drawSpritePart(r, tp123, lR, tR, wR - lR, hR - tR,
                                      inst->x + (float)lR, inst->y + (float)tR,
                                      1.0f, 1.0f, 0xFFFFFFu, 1.0f);
        }
        
        int32_t tp124 = Renderer_resolveTPAGIndex(ctx->dataWin, 124, subimg);
        if (tp124 >= 0) {
            r->vtable->drawSpritePart(r, tp124, lR, tR, wR - lR, hR - tR,
                                      inst->x + (float)lR, ibT + 6.0f,
                                      1.0f, 1.0f, 0xFFFFFFu, 1.0f);
        }
    }

    
    if (inst->x > (ibL - 5.0f) && inst->x < (ibR - 4.0f)) {
        Instance_setSelfVar(inst, topboneCache.drawn, RValue_makeReal(1.0));
        
        uint32_t fillCol = (blue == 1) ? 0xFFA914u : 0xFFFFFFu;
        r->vtable->drawRectangle(r, inst->x + 3.0f, inst->y,
                                 inst->x + 9.0f, ibT + 10.0f,
                                 fillCol, 1.0f, false);
    }

    
    Instance* heart = (topboneCache.objHeart >= 0) ? findInstanceByObject(runner, topboneCache.objHeart) : NULL;
    GMLReal invc = (topboneCache.gInvc >= 0) ? globalReal(ctx, topboneCache.gInvc) : 0.0;
    if (heart && fabsf(heart->x - inst->x) < 15.0f && invc < 1.0) {
        BuiltinFunc cr = VMBuiltins_find("collision_rectangle");
        if (cr) {
            RValue args[7] = {
                RValue_makeReal(inst->x + 3.0), RValue_makeReal(inst->y),
                RValue_makeReal(inst->x + 9.0), RValue_makeReal(ibT + 10.0),
                RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
            };
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = cr(ctx, args, 7);
            ctx->currentInstance = saved;
            if (RValue_toInt32(res) >= 0) {
                Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
            }
            RValue_free(&res);
        }
    }

    
    if (inst->x < (ibL - 10.0f) && inst->hspeed < 0.0f) { Runner_destroyInstance(runner, inst); return; }
    if (inst->x > (ibR + 10.0f) && inst->hspeed > 0.0f) { Runner_destroyInstance(runner, inst); return; }
}




static struct {
    int32_t movinged;
    int32_t gIdealborder, gInvc, gBorder;
    int32_t objTime, objHeart, objSuperbone;
    int32_t timeUpVar, superboneXVar;
    bool ready;
} coolbusCache = { .ready = false };

static void initCoolbusCache(VMContext* ctx, DataWin* dw) {
    coolbusCache.movinged = findSelfVarId(dw, "movinged");
    coolbusCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    coolbusCache.gInvc        = findGlobalVarId(ctx, "invc");
    coolbusCache.gBorder      = findGlobalVarId(ctx, "border");
    coolbusCache.objTime      = findObjectIndex(dw, "obj_time");
    coolbusCache.objHeart     = findObjectIndex(dw, "obj_heart");
    coolbusCache.objSuperbone = 640;  
    coolbusCache.timeUpVar    = findSelfVarId(dw, "up");
    coolbusCache.superboneXVar = -1;  
    coolbusCache.ready = (coolbusCache.movinged >= 0 && coolbusCache.gIdealborder >= 0);
}

static void native_coolbus_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!coolbusCache.ready || runner->renderer == NULL) return;

    
    native_drawSelfBorder(ctx, runner, inst);

    float ibL = (float)topboneGetBorder(ctx, 0);
    float ibR = (float)topboneGetBorder(ctx, 1);
    float ibB = (float)topboneGetBorder(ctx, 3);

    
    if (inst->x < (ibL - 100.0f) && inst->hspeed < 0.0f) { Runner_destroyInstance(runner, inst); return; }
    if (inst->x > (ibR + 100.0f) && inst->hspeed > 0.0f) { Runner_destroyInstance(runner, inst); return; }

    
    Instance* superbone = findInstanceByObject(runner, coolbusCache.objSuperbone);
    if (superbone != NULL && inst->x < ibR) {
        Instance* time = (coolbusCache.objTime >= 0) ? findInstanceByObject(runner, coolbusCache.objTime) : NULL;
        Instance* heart = (coolbusCache.objHeart >= 0) ? findInstanceByObject(runner, coolbusCache.objHeart) : NULL;
        int32_t movinged = selfInt(inst, coolbusCache.movinged);
        bool timeUp = false;
        if (time && coolbusCache.timeUpVar >= 0) {
            timeUp = (selfInt(time, coolbusCache.timeUpVar) != 0);
        }
        if (timeUp && movinged == 0 && heart != NULL &&
            heart->x < (superbone->x + 20.0f) && heart->y > 50.0f) {
            
            if (coolbusCache.gBorder >= 0)
                globalSet(ctx, coolbusCache.gBorder, RValue_makeReal(51.0));
            
            if (heart->y < 270.0f) {
                int32_t snapped = (int32_t)floorf(((heart->y - 20.0f) / 5.0f) + 0.5f) * 5;
                int64_t k = ((int64_t)coolbusCache.gIdealborder << 32) | (uint32_t)2;
                RValue nv = RValue_makeReal((GMLReal)snapped);
                ptrdiff_t idx = hmgeti(ctx->globalArrayMap, k);
                if (idx >= 0) { RValue_free(&ctx->globalArrayMap[idx].value); ctx->globalArrayMap[idx].value = nv; }
                else          { ArrayMapEntry e = { .key = k, .value = nv }; hmputs(ctx->globalArrayMap, e); }
            }
            
            if (coolbusCache.movinged >= 0) {
                for (int32_t i = 0; i < (int32_t)arrlen(runner->instances); i++) {
                    Instance* it = runner->instances[i];
                    if (it && it->objectIndex == 639 && it->active) {
                        Instance_setSelfVar(it, coolbusCache.movinged, RValue_makeReal(1.0));
                    }
                }
            }
            
            if (heart->vspeed >= -2.0f && heart->yprevious > heart->y) {
                heart->vspeed = -2.0f;
                Instance_computeSpeedFromComponents(heart);
            }
        }
    }

    
    Instance_setSelfVar(inst, coolbusCache.movinged, RValue_makeReal(0.0));

    
    GMLReal invc = (coolbusCache.gInvc >= 0) ? globalReal(ctx, coolbusCache.gInvc) : 0.0;
    Instance* heart2 = (coolbusCache.objHeart >= 0) ? findInstanceByObject(runner, coolbusCache.objHeart) : NULL;
    if (invc < 2.0 && heart2 != NULL && fabsf((heart2->x + 25.0f) - inst->x) < 50.0f) {
        BuiltinFunc cr = VMBuiltins_find("collision_rectangle");
        if (cr) {
            RValue args[7] = {
                RValue_makeReal(inst->x + 5.0), RValue_makeReal(inst->y + 10.0),
                RValue_makeReal(inst->x + 55.0), RValue_makeReal(ibB - 10.0),
                RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
            };
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = cr(ctx, args, 7);
            ctx->currentInstance = saved;
            if (RValue_toInt32(res) >= 0) {
                Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
            }
            RValue_free(&res);
        }
    }
}




static struct { int32_t anim; bool ready; } fgWaterfallCache = { .ready = false };
static void initFgWaterfallCache(DataWin* dw) {
    fgWaterfallCache.anim = findSelfVarId(dw, "anim");
    fgWaterfallCache.ready = (fgWaterfallCache.anim >= 0);
}
static void native_fgWaterfall_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!fgWaterfallCache.ready || runner->renderer == NULL || !runner->currentRoom) return;
    Renderer* r = runner->renderer;

    GMLReal anim = selfReal(inst, fgWaterfallCache.anim) + 3.0;
    int32_t sprite = inst->spriteIndex;
    for (int32_t i = 0; i < 20; i++) {
        Renderer_drawSpriteExt(r, sprite, 0, inst->x,
                               -210.0f + inst->y + (float)(i * 30) + (float)anim,
                               2.0f, 2.0f, 0.0f, 0xFFFFFFu, 0.2f);
    }
    if (anim > 180.0) anim -= 180.0;
    Instance_setSelfVar(inst, fgWaterfallCache.anim, RValue_makeReal(anim));

    
    float viewX = (float)runner->currentRoom->views[0].viewX;
    float viewW = (float)runner->currentRoom->views[0].viewWidth;
    float roomW = (float)runner->currentRoom->width;
    float myview = 0.0f;
    if (viewX > 0.0f && viewX < (roomW - viewW)) myview = viewX;
    if (viewX >= (roomW - viewW)) myview = roomW - viewW;
    inst->x = inst->xstart - myview * 0.5f;
}





typedef struct {
    int32_t single;   
    int32_t topLeft;  
    int32_t topMid;   
    int32_t topRight; 
    int32_t leftEdge; 
    int32_t midLeft;  
    int32_t topLeftCap; 
    int32_t middle;   
    int32_t rightEdge;
    int32_t botRight; 
    int32_t botMid;   
} WaterfallSpriteSet;

static void waterfallDraw(Renderer* r, Instance* inst, GMLReal siner,
                          const WaterfallSpriteSet* s) {
    int32_t subimg = (int32_t)(siner / 5.0);
    float ixs = inst->imageXscale;
    float iys = inst->imageYscale;

    if (ixs == 1.0f) {
        Renderer_drawSprite(r, s->single, subimg, inst->x, inst->y);
    } else if (ixs > 1.0f) {
        
        Renderer_drawSprite(r, s->topLeft, subimg, inst->x, inst->y);
        for (int32_t i = 1; (float)i < ixs + 1.0f; i++) {
            if ((float)i < ixs) {
                Renderer_drawSprite(r, s->topMid, subimg, inst->x + (float)(i * 20), inst->y);
            } else {
                Renderer_drawSprite(r, s->topRight, subimg, inst->x + (float)(i * 20) - 20.0f, inst->y);
                break;
            }
        }
    }

    if (iys > 1.0f && ixs == 1.0f) {
        for (int32_t i = 1; (float)i <= iys; i++) {
            Renderer_drawSprite(r, s->leftEdge, subimg, inst->x, inst->y + (float)(i * 20));
        }
    }

    if (iys > 1.0f && ixs > 1.0f) {
        for (int32_t j = 1; (float)j <= iys; j++) {
            if ((float)j < iys) {
                Renderer_drawSprite(r, s->midLeft, subimg, inst->x, inst->y + (float)(j * 20));
            }
            if ((float)j == iys) {
                Renderer_drawSprite(r, s->topLeftCap, subimg, inst->x, inst->y + (float)(j * 20) - 20.0f);
            }
            for (int32_t i = 1; (float)i <= ixs; i++) {
                if ((float)j < iys) {
                    if ((float)i == ixs) {
                        Renderer_drawSprite(r, s->rightEdge, subimg,
                                            inst->x + (float)(i * 20) - 20.0f,
                                            inst->y + (float)(j * 20));
                    } else {
                        Renderer_drawSprite(r, s->middle, subimg,
                                            inst->x + (float)(i * 20),
                                            inst->y + (float)(j * 20));
                    }
                }
                if ((float)j == iys) {
                    if ((float)i == ixs) {
                        Renderer_drawSprite(r, s->botRight, subimg,
                                            inst->x + (float)(i * 20) - 20.0f,
                                            inst->y + (float)(j * 20) - 20.0f);
                    } else {
                        Renderer_drawSprite(r, s->botMid, subimg,
                                            inst->x + (float)(i * 20),
                                            inst->y + (float)(j * 20) - 20.0f);
                    }
                }
            }
        }
    }
}

static struct { int32_t siner; bool ready; } waterfallCache = { .ready = false };
static void initWaterfallCache(DataWin* dw) {
    waterfallCache.siner = findSelfVarId(dw, "siner");
    waterfallCache.ready = (waterfallCache.siner >= 0);
}
static void native_waterfallWaterfall_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!waterfallCache.ready || runner->renderer == NULL) return;
    GMLReal siner = selfReal(inst, waterfallCache.siner) + 1.0;
    Instance_setSelfVar(inst, waterfallCache.siner, RValue_makeReal(siner));
    
    static const WaterfallSpriteSet s = {
        .single = 1034, .topLeft = 1033, .topMid = 1032, .topRight = 1035,
        .leftEdge = 1030, .midLeft = 1029, .topLeftCap = 1026,
        .middle = 1030, .rightEdge = 1031, .botRight = 1028, .botMid = 1027
    };
    waterfallDraw(runner->renderer, inst, siner, &s);
}
static void native_brightwaterfall_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!waterfallCache.ready || runner->renderer == NULL) return;
    GMLReal siner = selfReal(inst, waterfallCache.siner) + 1.0;
    Instance_setSelfVar(inst, waterfallCache.siner, RValue_makeReal(siner));
    
    static const WaterfallSpriteSet s = {
        .single = 1034, .topLeft = 1038, .topMid = 1036, .topRight = 1037,
        .leftEdge = 1041, .midLeft = 1039, .topLeftCap = 1044,
        .middle = 1041, .rightEdge = 1040, .botRight = 1043, .botMid = 1042
    };
    waterfallDraw(runner->renderer, inst, siner, &s);
}




static struct {
    int32_t talkcounter, myinteract, snd;
    bool ready;
} glowfly1Cache = { .ready = false };
static void initGlowfly1Cache(DataWin* dw) {
    glowfly1Cache.talkcounter = findSelfVarId(dw, "talkcounter");
    glowfly1Cache.myinteract  = findSelfVarId(dw, "myinteract");
    glowfly1Cache.snd         = findSelfVarId(dw, "snd");
    glowfly1Cache.ready = (glowfly1Cache.talkcounter >= 0 && glowfly1Cache.myinteract >= 0);
}
static void native_glowfly1_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!glowfly1Cache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    int32_t subimg = (int32_t)inst->imageIndex;
    Renderer_drawSprite(r, inst->spriteIndex, subimg, inst->x, inst->y);
    Renderer_drawSpriteExt(r, inst->spriteIndex, subimg,
                           inst->x - 0.5f, inst->y - 0.5f, 1.5f, 1.5f, 0.0f, 0xFFFFFFu, 0.25f);
    GMLReal tc = selfReal(inst, glowfly1Cache.talkcounter) - 1.0;
    int32_t mi = selfInt(inst, glowfly1Cache.myinteract);
    if (mi == 1 && tc < 0.0) {
        int32_t snd_n = (rand() & 1) ? 19 : 18;  
        if (glowfly1Cache.snd >= 0)
            Instance_setSelfVar(inst, glowfly1Cache.snd, RValue_makeReal((GMLReal)snd_n));
        BuiltinFunc sp = VMBuiltins_find("snd_play");
        if (sp) {
            RValue arg = RValue_makeReal((GMLReal)snd_n);
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue rr = sp(ctx, &arg, 1);
            ctx->currentInstance = saved;
            RValue_free(&rr);
        }
        tc = 30.0;
        Instance_setSelfVar(inst, glowfly1Cache.myinteract, RValue_makeReal(0.0));
    }
    Instance_setSelfVar(inst, glowfly1Cache.talkcounter, RValue_makeReal(tc));
}




static struct {
    int32_t siner, gl, gl2;
    int32_t objDarknesspuzzle;
    int32_t dpGlowamtVar;
    bool ready;
} glowstoneCache = { .ready = false };
static void initGlowstoneCache(DataWin* dw) {
    glowstoneCache.siner = findSelfVarId(dw, "siner");
    glowstoneCache.gl    = findSelfVarId(dw, "gl");
    glowstoneCache.gl2   = findSelfVarId(dw, "gl2");
    glowstoneCache.objDarknesspuzzle = findObjectIndex(dw, "obj_darknesspuzzle");
    glowstoneCache.dpGlowamtVar = findSelfVarId(dw, "glowamt");
    glowstoneCache.ready = (glowstoneCache.siner >= 0);
}
static void native_glowstone_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!glowstoneCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    float gl = 0.0f;
    Instance* dp = (glowstoneCache.objDarknesspuzzle >= 0)
                   ? findInstanceByObject(runner, glowstoneCache.objDarknesspuzzle) : NULL;
    if (dp != NULL && glowstoneCache.dpGlowamtVar >= 0) {
        gl = (float)selfReal(dp, glowstoneCache.dpGlowamtVar);
    }
    if (glowstoneCache.gl >= 0)
        Instance_setSelfVar(inst, glowstoneCache.gl, RValue_makeReal((GMLReal)gl));

    GMLReal siner = selfReal(inst, glowstoneCache.siner);
    if (gl > 0.1f) {
        float gl2v = gl + (float)(GMLReal_sin(siner / 10.0) / 6.0);
        if (glowstoneCache.gl2 >= 0)
            Instance_setSelfVar(inst, glowstoneCache.gl2, RValue_makeReal((GMLReal)gl2v));
        inst->imageAlpha = gl;

        
        
        float haloAlpha = gl2v / 3.0f;
        if (haloAlpha < 0.0f) haloAlpha = 0.0f;
        if (haloAlpha > 1.0f) haloAlpha = 1.0f;
        
        int32_t savedPrec = r->circlePrecision;
        r->circlePrecision = 12;
        r->vtable->drawCircle(r, inst->x + 10.0f, inst->y + 10.0f, gl2v * 15.0f,
                              0xFF00FFu, haloAlpha, false, r->circlePrecision);
        r->vtable->drawCircle(r, inst->x + 10.0f, inst->y + 10.0f, gl2v * 20.0f,
                              0xFF00FFu, haloAlpha, false, r->circlePrecision);
        r->vtable->drawCircle(r, inst->x + 10.0f, inst->y + 10.0f, gl2v * 25.0f,
                              0xFF00FFu, haloAlpha, false, r->circlePrecision);
        r->vtable->drawCircle(r, inst->x + 10.0f, inst->y + 10.0f, gl2v * 30.0f,
                              0xFF00FFu, haloAlpha, false, r->circlePrecision);
        r->circlePrecision = savedPrec;
    }
    siner += 1.0;
    Instance_setSelfVar(inst, glowstoneCache.siner, RValue_makeReal(siner));

    Renderer_drawSpriteExt(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, gl);
}




static struct {
    int32_t x1, x2, y1, y2, glowamt;
    int32_t gPlot;
    int32_t objMainchara;
    bool ready;
} darknesspuzzleCache = { .ready = false };
static void initDarknesspuzzleCache(VMContext* ctx, DataWin* dw) {
    darknesspuzzleCache.x1 = findSelfVarId(dw, "x1");
    darknesspuzzleCache.x2 = findSelfVarId(dw, "x2");
    darknesspuzzleCache.y1 = findSelfVarId(dw, "y1");
    darknesspuzzleCache.y2 = findSelfVarId(dw, "y2");
    darknesspuzzleCache.glowamt = findSelfVarId(dw, "glowamt");
    darknesspuzzleCache.gPlot = findGlobalVarId(ctx, "plot");
    darknesspuzzleCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    darknesspuzzleCache.ready = (darknesspuzzleCache.glowamt >= 0);
}
static void native_darknesspuzzle_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!darknesspuzzleCache.ready || runner->renderer == NULL) return;
    
    if (findInstanceByObject(runner, 1576) == NULL) return;

    Instance* mc = (darknesspuzzleCache.objMainchara >= 0)
                   ? findInstanceByObject(runner, darknesspuzzleCache.objMainchara) : NULL;
    if (mc == NULL) return;

    float px1 = mc->x - 10.0f, py1 = mc->y - 5.0f;
    float px2 = mc->x + 30.0f, py2 = mc->y + 35.0f;
    if (darknesspuzzleCache.x1 >= 0) Instance_setSelfVar(inst, darknesspuzzleCache.x1, RValue_makeReal(px1));
    if (darknesspuzzleCache.x2 >= 0) Instance_setSelfVar(inst, darknesspuzzleCache.x2, RValue_makeReal(px2));
    if (darknesspuzzleCache.y1 >= 0) Instance_setSelfVar(inst, darknesspuzzleCache.y1, RValue_makeReal(py1));
    if (darknesspuzzleCache.y2 >= 0) Instance_setSelfVar(inst, darknesspuzzleCache.y2, RValue_makeReal(py2));

    GMLReal glowamt = selfReal(inst, darknesspuzzleCache.glowamt);
    Renderer_drawSpriteExt(runner->renderer, 1647, 0, px1, py1, 1.0f, 1.0f, 0.0f,
                           0xFFFFFFu, (float)glowamt);

    
    
    if (glowamt < 0.98) {
        GMLReal plot = (darknesspuzzleCache.gPlot >= 0) ? globalReal(ctx, darknesspuzzleCache.gPlot) : 0.0;
        glowamt += (plot > 117.0) ? 0.001 : 0.003;
        Instance_setSelfVar(inst, darknesspuzzleCache.glowamt, RValue_makeReal(glowamt));
    }
}









static void puddleDrawCustomExt(Renderer* r, DataWin* dw, Instance* puddleInst,
                                float x1, float x2, float y1, float y2,
                                int32_t sprite, int32_t subimg,
                                float xscale, float yscale, float alpha,
                                float dx, float dy) {
    
    
    if (sprite == 0) sprite = puddleInst->spriteIndex;
    if (sprite < 0 || (uint32_t)sprite >= dw->sprt.count) return;
    if (subimg == 0) subimg = (int32_t)puddleInst->imageIndex;
    if (xscale == 0.0f) xscale = 1.0f;
    if (yscale == 0.0f) yscale = 1.0f;

    
    
    
    Sprite* puddleSpr = &dw->sprt.sprites[puddleInst->spriteIndex];
    float sw = (float)puddleSpr->width * puddleInst->imageXscale;
    float sh = (float)puddleSpr->height * puddleInst->imageYscale;

    float l = 0.0f, t = 0.0f, w = sw, h = sh;
    float ll = (x1 - dx) + 1.0f;
    float tt = (y1 - dy) + 1.0f;
    float ww = (dx + w) - x2 - 1.0f;
    float hh = (dy + h) - y2 - 1.0f;
    if (ll > 0.0f) l += ll;
    if (tt > 0.0f) t += tt;
    if (ww > 0.0f) w -= ww;
    if (hh > 0.0f) h -= hh;

    int32_t lR = (int32_t)floorf(l + 0.5f);
    int32_t tR = (int32_t)floorf(t + 0.5f);
    int32_t wR = (int32_t)floorf(w + 0.5f);
    int32_t hR = (int32_t)floorf(h + 0.5f);

    
    Sprite* reflSpr = &dw->sprt.sprites[sprite];
    int32_t maxW = (int32_t)reflSpr->width;
    int32_t maxH = (int32_t)reflSpr->height;
    if (wR > maxW) wR = maxW;
    if (hR > maxH) hR = maxH;

    if (wR > 0 && hR > 0 && lR < wR && tR < hR) {
        int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, sprite, subimg);
        if (tpagIndex >= 0) {
            r->vtable->drawSpritePart(r, tpagIndex, lR, tR, wR - lR, hR - tR,
                                      dx + (float)lR, dy + (float)tR,
                                      xscale, yscale, 0xFFFFFFu, alpha);
        }
    }
}

static struct {
    int32_t death, value, ndtry, sprito, sprito2, simage;
    int32_t dsprite, usprite, lsprite, rsprite;
    int32_t objMainchara, objMkid;
    int32_t gFlag;
    bool ready;
} puddleCache = { .ready = false };
static void initPuddleCache(VMContext* ctx, DataWin* dw) {
    puddleCache.death   = findSelfVarId(dw, "death");
    puddleCache.value   = findSelfVarId(dw, "value");
    puddleCache.ndtry   = findSelfVarId(dw, "ndtry");
    puddleCache.sprito  = findSelfVarId(dw, "sprito");
    puddleCache.sprito2 = findSelfVarId(dw, "sprito2");
    puddleCache.simage  = findSelfVarId(dw, "simage");
    puddleCache.dsprite = findSelfVarId(dw, "dsprite");
    puddleCache.usprite = findSelfVarId(dw, "usprite");
    puddleCache.lsprite = findSelfVarId(dw, "lsprite");
    puddleCache.rsprite = findSelfVarId(dw, "rsprite");
    puddleCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    puddleCache.objMkid      = 1117;  
    puddleCache.gFlag        = findGlobalVarId(ctx, "flag");
    puddleCache.ready = (puddleCache.dsprite >= 0 && puddleCache.usprite >= 0 &&
                         puddleCache.lsprite >= 0 && puddleCache.rsprite >= 0);
}

static inline float instSpriteHeight(DataWin* dw, Instance* inst) {
    uint32_t si = (uint32_t)inst->spriteIndex;
    if (si >= dw->sprt.count) return 0.0f;
    return (float)dw->sprt.sprites[si].height * inst->imageYscale;
}

static void native_puddle_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!puddleCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    DataWin* dw = ctx->dataWin;

    
    if (findInstanceByObject(runner, 1576) == NULL) return;

    
    float bbL = inst->x, bbT = inst->y, bbR = inst->x, bbB = inst->y;
    uint32_t si = (uint32_t)inst->spriteIndex;
    if (si < dw->sprt.count) {
        Sprite* spr = &dw->sprt.sprites[si];
        float originX = (float)spr->originX;
        float originY = (float)spr->originY;
        bbL = inst->x + ((float)spr->marginLeft   - originX) * inst->imageXscale;
        bbT = inst->y + ((float)spr->marginTop    - originY) * inst->imageYscale;
        bbR = inst->x + ((float)spr->marginRight  - originX) * inst->imageXscale;
        bbB = inst->y + ((float)spr->marginBottom - originY) * inst->imageYscale;
    }
    r->vtable->drawRectangle(r, bbL, bbT, bbR, bbB, 0x000000u, 1.0f, false);

    int32_t ndtry = 0;
    Instance* mainchara = (puddleCache.objMainchara >= 0)
                          ? findInstanceByObject(runner, puddleCache.objMainchara) : NULL;

    
    Instance* mkid = findInstanceByObject(runner, puddleCache.objMkid);
    int32_t sprito2 = 0;
    if (mkid != NULL) {
        sprito2 = mkid->spriteIndex;
        int32_t mkR = (puddleCache.rsprite >= 0) ? selfInt(mkid, puddleCache.rsprite) : -1;
        int32_t mkD = (puddleCache.dsprite >= 0) ? selfInt(mkid, puddleCache.dsprite) : -1;
        int32_t mkU = (puddleCache.usprite >= 0) ? selfInt(mkid, puddleCache.usprite) : -1;
        int32_t mkL = (puddleCache.lsprite >= 0) ? selfInt(mkid, puddleCache.lsprite) : -1;

        if (mkid->spriteIndex == mkR) sprito2 = 1468;
        if (mkid->spriteIndex == 1489) sprito2 = 1469;
        if (mkid->spriteIndex == mkD) sprito2 = 1466;
        if (mkid->spriteIndex == mkU) sprito2 = 1470;
        if (mkid->spriteIndex == mkL) sprito2 = 1467;

        if (mainchara != NULL && mkid->depth > mainchara->depth) {
            float sprH = instSpriteHeight(dw, mkid);
            puddleDrawCustomExt(r, dw, inst,
                                bbL, bbR, bbT, bbB - 1.0f,
                                sprito2, (int32_t)mkid->imageIndex,
                                1.0f, 1.0f, 0.4f,
                                mkid->x, mkid->y + sprH);
        } else {
            ndtry = 1;
        }
    }
    if (puddleCache.sprito2 >= 0)
        Instance_setSelfVar(inst, puddleCache.sprito2, RValue_makeReal((GMLReal)sprito2));

    
    if (mainchara != NULL) {
        int32_t sprito = mainchara->spriteIndex;
        int32_t mcR = (puddleCache.rsprite >= 0) ? selfInt(mainchara, puddleCache.rsprite) : -1;
        int32_t mcD = (puddleCache.dsprite >= 0) ? selfInt(mainchara, puddleCache.dsprite) : -1;
        int32_t mcU = (puddleCache.usprite >= 0) ? selfInt(mainchara, puddleCache.usprite) : -1;
        int32_t mcL = (puddleCache.lsprite >= 0) ? selfInt(mainchara, puddleCache.lsprite) : -1;

        if (mcR == 1133) {
            if (mainchara->spriteIndex == 1133) sprito = 1092;
            if (mainchara->spriteIndex == 1131) sprito = 1088;
            if (mainchara->spriteIndex == 1132) sprito = 1090;
            if (mainchara->spriteIndex == 1134) sprito = 1091;
        }
        if (mcR == 1106) {
            if (mainchara->spriteIndex == 1106) sprito = 1102;
            if (mainchara->spriteIndex == 1104) sprito = 1100;
            if (mainchara->spriteIndex == 1105) sprito = 1101;
            if (mainchara->spriteIndex == 1107) sprito = 1103;
        }

        int32_t death = (puddleCache.death >= 0) ? selfInt(inst, puddleCache.death) : 0;
        if (death == 1) {
            if (mainchara->spriteIndex == 1133) sprito = 1111;
            if (mainchara->spriteIndex == 1131) sprito = 1109;
            if (mainchara->spriteIndex == 1132) sprito = 1115;
            if (mainchara->spriteIndex == 1134) sprito = 1113;
        }

        int32_t simage = (int32_t)mainchara->imageIndex;
        if (puddleCache.sprito >= 0) Instance_setSelfVar(inst, puddleCache.sprito, RValue_makeReal((GMLReal)sprito));
        if (puddleCache.simage >= 0) Instance_setSelfVar(inst, puddleCache.simage, RValue_makeReal((GMLReal)simage));

        int32_t flag85 = 0;
        if (puddleCache.gFlag >= 0) {
            int64_t k = ((int64_t)puddleCache.gFlag << 32) | (uint32_t)85;
            ptrdiff_t p = hmgeti(ctx->globalArrayMap, k);
            if (p >= 0) flag85 = (int32_t)RValue_toReal(ctx->globalArrayMap[p].value);
        }

        float mcSpriteH = instSpriteHeight(dw, mainchara);
        if (flag85 == 0) {
            puddleDrawCustomExt(r, dw, inst,
                                bbL, bbR, bbT, bbB - 1.0f,
                                sprito, simage,
                                1.0f, 1.0f, 0.4f,
                                mainchara->x, mainchara->y + mcSpriteH);
        } else {
            int32_t value = 0;
            if (mainchara->spriteIndex == mcD) value = 3;
            if (mainchara->spriteIndex == mcU) value = 16;
            if (mainchara->spriteIndex == mcR) value = 10;
            if (mainchara->spriteIndex == mcL) value = 9;
            if (puddleCache.value >= 0) Instance_setSelfVar(inst, puddleCache.value, RValue_makeReal((GMLReal)value));
            puddleDrawCustomExt(r, dw, inst,
                                bbL, bbR, bbT, bbB - 1.0f,
                                sprito, simage,
                                1.0f, 1.0f, 0.4f,
                                mainchara->x - (float)value, mainchara->y + 30.0f);
        }
    }

    
    if (ndtry == 1 && mkid != NULL) {
        float sprH = instSpriteHeight(dw, mkid);
        puddleDrawCustomExt(r, dw, inst,
                            bbL, bbR, bbT, bbB - 1.0f,
                            sprito2, (int32_t)mkid->imageIndex,
                            1.0f, 1.0f, 0.4f,
                            mkid->x, mkid->y + sprH);
    }
    if (puddleCache.ndtry >= 0)
        Instance_setSelfVar(inst, puddleCache.ndtry, RValue_makeReal((GMLReal)ndtry));

    
    Renderer_drawSprite(r, inst->spriteIndex, (int32_t)inst->imageIndex, inst->x, inst->y);

    
}




#define SPEAR_MAX_ACTIVE_VARIDS 16
static struct {
    int32_t deg, x1, y1, x2, y2, r_v, rot, active, move, col;
    int32_t ramt, rspeed, rdir, idealrot, gax;
    int32_t objUndyneaActor, objMainchara;
    int32_t actor_hspeed_builtin;  
    bool ready;
} undyneSpearCache = { .ready = false };
static void initUndyneSpearCache(DataWin* dw) {
    undyneSpearCache.deg     = findSelfVarId(dw, "deg");
    undyneSpearCache.x1      = findSelfVarId(dw, "x1");
    undyneSpearCache.y1      = findSelfVarId(dw, "y1");
    undyneSpearCache.x2      = findSelfVarId(dw, "x2");
    undyneSpearCache.y2      = findSelfVarId(dw, "y2");
    undyneSpearCache.r_v     = findSelfVarId(dw, "r");
    undyneSpearCache.rot     = findSelfVarId(dw, "rot");
    undyneSpearCache.active  = findSelfVarId(dw, "active");
    undyneSpearCache.move    = findSelfVarId(dw, "move");
    undyneSpearCache.col     = findSelfVarId(dw, "col");
    undyneSpearCache.ramt    = findSelfVarId(dw, "ramt");
    undyneSpearCache.rspeed  = findSelfVarId(dw, "rspeed");
    undyneSpearCache.rdir    = findSelfVarId(dw, "rdir");
    undyneSpearCache.idealrot = findSelfVarId(dw, "idealrot");
    undyneSpearCache.gax     = findSelfVarId(dw, "gax");
    undyneSpearCache.objUndyneaActor = findObjectIndex(dw, "obj_undynea_actor");
    undyneSpearCache.objMainchara    = findObjectIndex(dw, "obj_mainchara");
    undyneSpearCache.ready = (undyneSpearCache.rot >= 0 && undyneSpearCache.active >= 0 &&
                              undyneSpearCache.r_v >= 0);
}
static void native_undynespear_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!undyneSpearCache.ready || runner->renderer == NULL || !runner->currentRoom) return;
    Renderer* r_ren = runner->renderer;

    GMLReal rot = selfReal(inst, undyneSpearCache.rot);
    GMLReal rad = rot * (M_PI / 180.0);
    GMLReal r_v = selfReal(inst, undyneSpearCache.r_v);
    GMLReal x1 = inst->x + (r_v * GMLReal_cos(rad));
    GMLReal y1 = inst->y - (r_v * GMLReal_sin(rad));

    if (undyneSpearCache.deg >= 0) Instance_setSelfVar(inst, undyneSpearCache.deg, RValue_makeReal(rad));
    if (undyneSpearCache.x1 >= 0)  Instance_setSelfVar(inst, undyneSpearCache.x1,  RValue_makeReal(x1));
    if (undyneSpearCache.y1 >= 0)  Instance_setSelfVar(inst, undyneSpearCache.y1,  RValue_makeReal(y1));
    if (undyneSpearCache.x2 >= 0)  Instance_setSelfVar(inst, undyneSpearCache.x2,  RValue_makeReal(inst->x));
    if (undyneSpearCache.y2 >= 0)  Instance_setSelfVar(inst, undyneSpearCache.y2,  RValue_makeReal(inst->y));

    int32_t active = selfInt(inst, undyneSpearCache.active);
    if (inst->imageAlpha < 1.0f && active != 0) inst->imageAlpha += 0.1f;

    Renderer_drawSpriteExt(r_ren, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, 1.0f, 1.0f, (float)rot, 0xFFFFFFu, inst->imageAlpha);

    
    GMLReal move = (undyneSpearCache.move >= 0) ? selfReal(inst, undyneSpearCache.move) : 0.0;
    if (findInstanceByObject(runner, 1119) != NULL) {
        Instance* actor = (undyneSpearCache.objUndyneaActor >= 0)
                          ? findInstanceByObject(runner, undyneSpearCache.objUndyneaActor) : NULL;
        if (actor) move = (GMLReal)actor->hspeed;
    }
    inst->x += (float)(move / 3.0);
    if (undyneSpearCache.move >= 0) Instance_setSelfVar(inst, undyneSpearCache.move, RValue_makeReal(move));

    if (active == 1) {
        GMLReal col = selfReal(inst, undyneSpearCache.col);
        BuiltinFunc cr = VMBuiltins_find("collision_rectangle");
        if (cr) {
            RValue args[7] = {
                RValue_makeReal(x1), RValue_makeReal(y1),
                RValue_makeReal(inst->x), RValue_makeReal(inst->y),
                RValue_makeReal(1576.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
            };
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = cr(ctx, args, 7);
            ctx->currentInstance = saved;
            col = (RValue_toInt32(res) >= 0) ? (col + 1.0) : 0.0;
            RValue_free(&res);
        }
        if (col == 2.0) {
            Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 2);
        }
        rot = inst->direction;
        Instance_setSelfVar(inst, undyneSpearCache.col, RValue_makeReal(col));
    } else if (active == 4) {
        GMLReal rspeed = inst->speed;
        GMLReal rdir = inst->direction;
        GMLReal ramt = selfReal(inst, undyneSpearCache.ramt);
        rot += ramt;

        if (ramt > 0.0) {
            ramt -= 2.0;
        } else {
            ramt = 0.0;
            
            Instance* mc = (undyneSpearCache.objMainchara >= 0)
                           ? findInstanceByObject(runner, undyneSpearCache.objMainchara) : NULL;
            GMLReal gax = (undyneSpearCache.gax >= 0) ? selfReal(inst, undyneSpearCache.gax) : 0.0;
            if (mc) {
                BuiltinFunc mtp = VMBuiltins_find("move_towards_point");
                if (mtp) {
                    RValue args[3] = {
                        RValue_makeReal(mc->x + 7.0 + gax),
                        RValue_makeReal(mc->y + 15.0),
                        RValue_makeReal(0.1)
                    };
                    Instance* saved = (Instance*)ctx->currentInstance;
                    ctx->currentInstance = inst;
                    RValue res = mtp(ctx, args, 3);
                    ctx->currentInstance = saved;
                    RValue_free(&res);
                }
            }
            GMLReal idealrot = inst->direction;
            rot = GMLReal_fmod(rot, 360.0);
            if ((rot - idealrot) > 12.0) rot -= 4.0;
            if ((rot - idealrot) > 6.0)  rot -= 2.0;
            if ((rot - idealrot) > 3.0)  rot -= 1.0;
            if ((rot - idealrot) < -3.0) rot += 1.0;
            if ((rot - idealrot) < -6.0) rot += 2.0;
            if ((rot - idealrot) < -12.0) rot += 4.0;
            if (undyneSpearCache.idealrot >= 0)
                Instance_setSelfVar(inst, undyneSpearCache.idealrot, RValue_makeReal(idealrot));
        }
        inst->speed = (float)rspeed;
        inst->direction = (float)rdir;
        Instance_computeComponentsFromSpeed(inst);
        if (undyneSpearCache.ramt >= 0)   Instance_setSelfVar(inst, undyneSpearCache.ramt, RValue_makeReal(ramt));
        if (undyneSpearCache.rspeed >= 0) Instance_setSelfVar(inst, undyneSpearCache.rspeed, RValue_makeReal(rspeed));
        if (undyneSpearCache.rdir >= 0)   Instance_setSelfVar(inst, undyneSpearCache.rdir, RValue_makeReal(rdir));
    } else if (active == 0) {
        inst->imageAlpha -= 0.05f;
        if (inst->imageAlpha < 0.01f) {
            active = 2;
        }
    }
    if (active == 2) {
        Runner_destroyInstance(runner, inst);
    }
    
    if (inst->y > (float)runner->currentRoom->height || inst->x > (float)runner->currentRoom->width) {
        active = 0;
    }
    Instance_setSelfVar(inst, undyneSpearCache.rot,    RValue_makeReal(rot));
    Instance_setSelfVar(inst, undyneSpearCache.active, RValue_makeReal((GMLReal)active));
}





static struct {
    int32_t pollenalpha, truepollenalpha, pollenx, polleny, pollensize, pollenhspeed, pollenvspeed;
    bool ready;
} pollenerCache = { .ready = false };
static void initPollenerCache(DataWin* dw) {
    pollenerCache.pollenalpha    = findSelfVarId(dw, "pollenalpha");
    pollenerCache.truepollenalpha = findSelfVarId(dw, "truepollenalpha");
    pollenerCache.pollenx        = findSelfVarId(dw, "pollenx");
    pollenerCache.polleny        = findSelfVarId(dw, "polleny");
    pollenerCache.pollensize     = findSelfVarId(dw, "pollensize");
    pollenerCache.pollenhspeed   = findSelfVarId(dw, "pollenhspeed");
    pollenerCache.pollenvspeed   = findSelfVarId(dw, "pollenvspeed");
    pollenerCache.ready = (pollenerCache.pollenalpha >= 0 && pollenerCache.pollenx >= 0 &&
                           pollenerCache.polleny >= 0);
}
static void native_pollener_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!pollenerCache.ready || !runner->currentRoom || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    float roomW = (float)runner->currentRoom->width;
    float roomH = (float)runner->currentRoom->height;
    
    int32_t savedPrec = r->circlePrecision;
    r->circlePrecision = 4;
    
    uint32_t yellow = 0x00FFFFu;

    for (int32_t i = 0; i < 100; i++) {
        GMLReal pa  = RValue_toReal(selfArrayGet(inst, pollenerCache.pollenalpha, i)) + 0.03;
        GMLReal tpa = pa;
        if (pa >= 2.0) tpa = 4.0 - pa;

        GMLReal phs = RValue_toReal(selfArrayGet(inst, pollenerCache.pollenhspeed, i));
        GMLReal pvs = RValue_toReal(selfArrayGet(inst, pollenerCache.pollenvspeed, i));
        GMLReal px  = RValue_toReal(selfArrayGet(inst, pollenerCache.pollenx, i)) + (phs * tpa) / 4.0;
        GMLReal py  = RValue_toReal(selfArrayGet(inst, pollenerCache.polleny, i)) + (pvs * tpa) / 4.0;
        GMLReal size = RValue_toReal(selfArrayGet(inst, pollenerCache.pollensize, i));

        if (tpa <= 0.0) {
            px = (double)rand() / (double)RAND_MAX * (double)roomW;
            py = (double)rand() / (double)RAND_MAX * (double)roomH;
            size = (double)rand() / (double)RAND_MAX * 3.0 + 1.0;
            phs = (double)rand() / (double)RAND_MAX * 2.0 - 1.0;
            pvs = (double)rand() / (double)RAND_MAX * 2.0 - 1.0;
            pa = 0.0;
            selfArraySet(inst, pollenerCache.pollensize,   i, RValue_makeReal(size));
            selfArraySet(inst, pollenerCache.pollenhspeed, i, RValue_makeReal(phs));
            selfArraySet(inst, pollenerCache.pollenvspeed, i, RValue_makeReal(pvs));
        }

        
        float pollenA = (float)tpa;
        if (pollenA > 0.0f && size > 0.0) {
            if (pollenA > 1.0f) pollenA = 1.0f;
            r->vtable->drawCircle(r, (float)px, (float)py, (float)size,
                                  yellow, pollenA, false, 4);
        }

        selfArraySet(inst, pollenerCache.pollenalpha,    i, RValue_makeReal(pa));
        selfArraySet(inst, pollenerCache.truepollenalpha, i, RValue_makeReal(tpa));
        selfArraySet(inst, pollenerCache.pollenx,         i, RValue_makeReal(px));
        selfArraySet(inst, pollenerCache.polleny,         i, RValue_makeReal(py));
    }
    r->circlePrecision = savedPrec;
}





static struct {
    int32_t xaround, inactive, greenbright;
    bool ready;
} hotlandsignCache = { .ready = false };
static void initHotlandsignCache(DataWin* dw) {
    hotlandsignCache.xaround     = findSelfVarId(dw, "xaround");
    hotlandsignCache.inactive    = findSelfVarId(dw, "inactive");
    hotlandsignCache.greenbright = findSelfVarId(dw, "greenbright");
    hotlandsignCache.ready = (hotlandsignCache.xaround >= 0 && hotlandsignCache.inactive >= 0);
}
static void native_hotlandsign_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!hotlandsignCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    r->vtable->drawRectangle(r, inst->x - 12.0f, inst->y - 12.0f,
                             inst->x + 493.0f, inst->y + 52.0f,
                             0x000000u, 1.0f, false);

    int32_t inactive = selfInt(inst, hotlandsignCache.inactive);
    GMLReal xaround = selfReal(inst, hotlandsignCache.xaround);
    int32_t subimg = (int32_t)inst->imageIndex;

    
    int32_t tp1963 = Renderer_resolveTPAGIndex(ctx->dataWin, 1963, subimg);
    if (tp1963 >= 0) {
        float a = (inactive == 1) ? 0.5f : 1.0f;
        r->vtable->drawSpritePart(r, tp1963, (int32_t)(0.0 - xaround), 0, 60, 5,
                                  inst->x, inst->y, 8.0f, 8.0f, 0x0000FFu, a);
    }

    
    for (int32_t i = 0; i < 60; i++) {
        for (int32_t g = 0; g < 5; g++) {
            Renderer_drawSprite(r, 1964, 0, inst->x + (float)(i * 8), inst->y + (float)(g * 8));
        }
    }

    
    
    
    
    int32_t greenbright = (hotlandsignCache.greenbright >= 0)
                          ? selfInt(inst, hotlandsignCache.greenbright) : 0;
    struct { float dx, dy, dw, dh; int32_t highlightAt; } borders[3] = {
        { -10.0f, -10.0f, 490.0f, 50.0f, 1 },  
        { -11.0f, -11.0f, 491.0f, 51.0f, 3 },  
        { -12.0f, -12.0f, 492.0f, 52.0f, 5 },  
    };
    if (r->vtable->drawRoundrect != NULL) {
        for (int32_t b = 0; b < 3; b++) {
            uint32_t col = (greenbright == borders[b].highlightAt) ? 0x00FF00u : 0x008000u;
            r->vtable->drawRoundrect(r,
                                     inst->x + borders[b].dx, inst->y + borders[b].dy,
                                     inst->x + borders[b].dw, inst->y + borders[b].dh,
                                     10.0f, 10.0f,   
                                     col, 1.0f, true, r->circlePrecision);
        }
    }
    (void)runner;
}




static struct {
    int32_t xx, lastx;
    bool ready;
} paralavaCache = { .ready = false };
static void initParalavaCache(DataWin* dw) {
    paralavaCache.xx    = findSelfVarId(dw, "xx");
    paralavaCache.lastx = findSelfVarId(dw, "lastx");
    paralavaCache.ready = (paralavaCache.xx >= 0);
}
static void native_hotlandparalava_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!paralavaCache.ready || runner->renderer == NULL || !runner->currentRoom) return;
    Renderer* r = runner->renderer;

    float roomW = (float)runner->currentRoom->width;
    float roomH = (float)runner->currentRoom->height;
    int32_t maximum = (int32_t)(roomW / 20.0f);

    
    
    static const struct { int32_t idx; float yOff; GMLReal xInc; float alpha; } layers[5] = {
        { 4, 80.0f, 0.25, 0.5f  },
        { 3, 68.0f, 0.5,  0.75f },
        { 2, 54.0f, 0.8, -1.0f  },  
        { 1, 38.0f, 0.9, -1.0f  },
        { 0, 20.0f, 1.0, -1.0f  }
    };
    float lastx = 0.0f;
    for (int32_t L = 0; L < 5; L++) {
        int32_t idx = layers[L].idx;
        RValue xxVal = selfArrayGet(inst, paralavaCache.xx, idx);
        GMLReal xx = RValue_toReal(xxVal) + layers[L].xInc;
        float baseY = roomH - layers[L].yOff;
        for (int32_t i = -1; i < maximum + 1; i++) {
            float px = (float)(i * 20) + (float)xx;
            if (layers[L].alpha >= 0.0f) {
                Renderer_drawSpriteExt(r, 1966, 0, px, baseY, 1.0f, 1.0f, 0.0f,
                                       0xFFFFFFu, layers[L].alpha);
            } else {
                Renderer_drawSprite(r, 1966, 0, px, baseY);
            }
            lastx = px;
        }
        if (xx >= 20.0) xx -= 20.0;
        selfArraySet(inst, paralavaCache.xx, idx, RValue_makeReal(xx));
    }
    if (paralavaCache.lastx >= 0)
        Instance_setSelfVar(inst, paralavaCache.lastx, RValue_makeReal((GMLReal)lastx));
}






static void native_sweatdrop_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (inst->imageAlpha < 1.0f) inst->imageAlpha += 0.05f;
    native_drawSelfBorder(ctx, runner, inst);
}










static struct {
    int32_t normal, create, destroy;
    bool ready;
} dummymissleCache = { .ready = false };
static void initDummymissleCache(DataWin* dw) {
    dummymissleCache.normal  = findSelfVarId(dw, "normal");
    dummymissleCache.create  = findSelfVarId(dw, "create");
    dummymissleCache.destroy = findSelfVarId(dw, "destroy");
    dummymissleCache.ready = (dummymissleCache.normal >= 0 && dummymissleCache.create >= 0);
}
static void native_dummymissle_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!dummymissleCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    int32_t subimg = (int32_t)inst->imageIndex;

    int32_t normal = selfInt(inst, dummymissleCache.normal);
    if (normal == 1) {
        Renderer_drawSpriteExt(r, inst->spriteIndex, subimg, inst->x, inst->y,
                               1.0f, 1.0f, inst->imageAngle, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 126, subimg, inst->x, inst->y,
                               1.0f, 1.0f, inst->imageAngle, 0xFFFFFFu, 1.0f);
    }

    GMLReal create = selfReal(inst, dummymissleCache.create);
    if (create < 6.0) {
        int32_t frame = 6 - (int32_t)create;
        Renderer_drawSpriteExt(r, 127, frame, inst->x, inst->y,
                               1.0f, 1.0f, inst->imageAngle, 0xFFFFFFu, 1.0f);
        create += 1.0;
        if (create >= 6.0) {
            Instance_setSelfVar(inst, dummymissleCache.normal, RValue_makeReal(1.0));
        }
        Instance_setSelfVar(inst, dummymissleCache.create, RValue_makeReal(create));
    }

    GMLReal destroy = (dummymissleCache.destroy >= 0) ? selfReal(inst, dummymissleCache.destroy) : 0.0;
    if (destroy >= 1.0) {
        float jx = (float)((double)rand()/(double)RAND_MAX * 2.0 - (double)rand()/(double)RAND_MAX * 2.0);
        float jy = (float)((double)rand()/(double)RAND_MAX * 2.0 - (double)rand()/(double)RAND_MAX * 2.0);
        inst->x += jx;
        inst->y += jy;
        if (destroy >= 2.0) {
            inst->imageXscale += 0.25f;
            inst->imageYscale += 0.25f;
        }
        Renderer_drawSpriteExt(r, 127, (int32_t)destroy - 1, inst->x, inst->y,
                               inst->imageXscale, inst->imageYscale, inst->imageAngle,
                               0xFFFFFFu, inst->imageAlpha);
        destroy += 1.0;
        if (destroy >= 8.0) {
            Runner_destroyInstance(runner, inst);
        } else {
            Instance_setSelfVar(inst, dummymissleCache.destroy, RValue_makeReal(destroy));
        }
    }
}






static struct {
    int32_t temx1, temy1, temx2, temy2, temno;
    int32_t objHeart;
    bool ready;
} temhandCache = { .ready = false };
static void initTemhandCache(DataWin* dw) {
    temhandCache.temx1 = findSelfVarId(dw, "temx1");
    temhandCache.temy1 = findSelfVarId(dw, "temy1");
    temhandCache.temx2 = findSelfVarId(dw, "temx2");
    temhandCache.temy2 = findSelfVarId(dw, "temy2");
    temhandCache.temno = findSelfVarId(dw, "temno");
    temhandCache.objHeart = findObjectIndex(dw, "obj_heart");
    temhandCache.ready = (temhandCache.temx1 >= 0 && temhandCache.temy1 >= 0 &&
                          temhandCache.temx2 >= 0 && temhandCache.temy2 >= 0 &&
                          temhandCache.temno >= 0);
}
static void native_temhand_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!temhandCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    float adjustx = 0.0f, adjusty = 0.0f;
    if (inst->direction == 0.0f || inst->direction == 180.0f) adjustx = 4.0f;
    if (inst->direction == 90.0f || inst->direction == 270.0f) adjusty = 4.0f;

    Renderer_drawSprite(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                        inst->x + adjustx, inst->y + adjusty);

    
    int32_t temno = selfInt(inst, temhandCache.temno);
    selfArraySet(inst, temhandCache.temx2, temno, RValue_makeReal(inst->x + 10.0));
    selfArraySet(inst, temhandCache.temy2, temno, RValue_makeReal(inst->y + 10.0));

    
    Instance* heart = (temhandCache.objHeart >= 0)
                      ? findInstanceByObject(runner, temhandCache.objHeart) : NULL;
    if (heart) {
        float xdif = inst->x - (heart->x + 2.0f);
        float ydif = inst->y - (heart->y + 6.0f);
        if (fabsf(xdif) < 20.0f && inst->alarm[0] > 5) inst->alarm[0] -= 2;
        if (fabsf(ydif) < 20.0f && inst->alarm[0] > 5) inst->alarm[0] -= 2;
        if (fabsf(xdif) < 10.0f && inst->alarm[0] > 4) inst->alarm[0] /= 2;
        if (fabsf(ydif) < 10.0f && inst->alarm[0] > 4) inst->alarm[0] /= 2;
    }

    
    BuiltinFunc cr = VMBuiltins_find("collision_rectangle");
    for (int32_t i = 0; i < 10; i++) {
        GMLReal x1 = RValue_toReal(selfArrayGet(inst, temhandCache.temx1, i));
        GMLReal y1 = RValue_toReal(selfArrayGet(inst, temhandCache.temy1, i));
        if (x1 <= 0.0) continue;
        GMLReal x2 = RValue_toReal(selfArrayGet(inst, temhandCache.temx2, i));
        GMLReal y2 = RValue_toReal(selfArrayGet(inst, temhandCache.temy2, i));
        r->vtable->drawRectangle(r, (float)x1, (float)y1, (float)x2, (float)y2,
                                 0xFFFFFFu, 1.0f, false);
        if (cr) {
            RValue args[7] = {
                RValue_makeReal(x1), RValue_makeReal(y1),
                RValue_makeReal(x2), RValue_makeReal(y2),
                RValue_makeReal(744.0), RValue_makeReal(0.0), RValue_makeReal(1.0)
            };
            Instance* saved = (Instance*)ctx->currentInstance;
            ctx->currentInstance = inst;
            RValue res = cr(ctx, args, 7);
            ctx->currentInstance = saved;
            if (RValue_toInt32(res) >= 0) {
                Runner_executeEvent(runner, inst, EVENT_OTHER, OTHER_USER0 + 1);
            }
            RValue_free(&res);
        }
    }
}




static struct {
    int32_t siner, fade, alpha, yoff;
    bool ready;
} boxsinerCache = { .ready = false };
static void initBoxsinerCache(DataWin* dw) {
    boxsinerCache.siner = findSelfVarId(dw, "siner");
    boxsinerCache.fade  = findSelfVarId(dw, "fade");
    boxsinerCache.alpha = findSelfVarId(dw, "alpha");
    boxsinerCache.yoff  = findSelfVarId(dw, "yoff");
    boxsinerCache.ready = (boxsinerCache.siner >= 0 && boxsinerCache.alpha >= 0);
}
static void native_boxsiner_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!boxsinerCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal siner = selfReal(inst, boxsinerCache.siner) + 1.0;
    Instance_setSelfVar(inst, boxsinerCache.siner, RValue_makeReal(siner));

    int32_t fade = (boxsinerCache.fade >= 0) ? selfInt(inst, boxsinerCache.fade) : 0;
    GMLReal alpha = selfReal(inst, boxsinerCache.alpha);
    if (fade == 1) alpha -= 0.01;
    Instance_setSelfVar(inst, boxsinerCache.alpha, RValue_makeReal(alpha));
    if (alpha <= 0.0) {
        Runner_destroyInstance(runner, inst);
        return;
    }

    
    
    
    
    float yoff = 0.0f;
    for (int32_t rep = 0; rep < 2; rep++) {
        for (int32_t i = 0; i < 6; i++) {
            float ysin = (float)(GMLReal_sin(((GMLReal)(i * 3) + siner / 2.0) / 8.0) * 20.0);
            r->vtable->drawRectangle(r,
                                     20.0f + (float)(i * 100),
                                     16.0f + yoff + ysin,
                                     20.0f + (float)((i + 1) * 100),
                                     136.0f + yoff + ysin,
                                     0x008000u, (float)alpha, true);
        }
        yoff = 120.0f;
    }
    if (boxsinerCache.yoff >= 0)
        Instance_setSelfVar(inst, boxsinerCache.yoff, RValue_makeReal((GMLReal)yoff));
}




static struct {
    int32_t mode, rotter, rot, rotmod, speedmod;
    int32_t partx, party, partrot, opartx, oparty, opartrot, go;
    int32_t check, fakegrav, dingus;
    int32_t gIdealborder, gFaceemotion;
    bool ready;
} maddumCache = { .ready = false };
static void initMaddumCache(VMContext* ctx, DataWin* dw) {
    maddumCache.mode = findSelfVarId(dw, "mode");
    maddumCache.rotter = findSelfVarId(dw, "rotter");
    maddumCache.rot = findSelfVarId(dw, "rot");
    maddumCache.rotmod = findSelfVarId(dw, "rotmod");
    maddumCache.speedmod = findSelfVarId(dw, "speedmod");
    maddumCache.partx = findSelfVarId(dw, "partx");
    maddumCache.party = findSelfVarId(dw, "party");
    maddumCache.partrot = findSelfVarId(dw, "partrot");
    maddumCache.opartx = findSelfVarId(dw, "opartx");
    maddumCache.oparty = findSelfVarId(dw, "oparty");
    maddumCache.opartrot = findSelfVarId(dw, "opartrot");
    maddumCache.go = findSelfVarId(dw, "go");
    maddumCache.check = findSelfVarId(dw, "check");
    maddumCache.fakegrav = findSelfVarId(dw, "fakegrav");
    maddumCache.dingus = findSelfVarId(dw, "dingus");
    maddumCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    maddumCache.gFaceemotion = findGlobalVarId(ctx, "faceemotion");
    maddumCache.ready = (maddumCache.mode >= 0 && maddumCache.partx >= 0 &&
                         maddumCache.party >= 0 && maddumCache.partrot >= 0);
}
static void native_maddumDrawer_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!maddumCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t mode = selfInt(inst, maddumCache.mode);
    int32_t subimg = (int32_t)inst->imageIndex;
    int32_t faceemotion = (maddumCache.gFaceemotion >= 0) ? (int32_t)globalReal(ctx, maddumCache.gFaceemotion) : 0;

    if (mode == 0) {
        GMLReal rotter = selfReal(inst, maddumCache.rotter) + 1.0;
        GMLReal speedmod = (maddumCache.speedmod >= 0) ? selfReal(inst, maddumCache.speedmod) : 0.0;
        rotter += speedmod / 2.0;
        GMLReal rot = GMLReal_sin(rotter / 6.0) * 30.0;
        GMLReal rotmod = (maddumCache.rotmod >= 0) ? selfReal(inst, maddumCache.rotmod) : 1.0;
        rot *= rotmod;
        Renderer_drawSpriteExt(r, 296, subimg, inst->x + 5.0f, inst->y + 75.0f,
                               2.0f, 2.0f, (float)-rot, 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 295, subimg, inst->x, inst->y + 35.0f + (float)(rot / 4.0),
                               2.0f, 2.0f, (float)(rot / 2.0), 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 294, subimg, inst->x + 5.0f, inst->y + 65.0f,
                               2.0f, 2.0f, (float)(rot / 3.0), 0xFFFFFFu, 1.0f);
        Renderer_drawSpriteExt(r, 293, faceemotion,
                               inst->x - (float)(rot / 3.0), inst->y + (float)(rot / 3.0),
                               2.0f, 2.0f, (float)rot, 0xFFFFFFu, 1.0f);
        Instance_setSelfVar(inst, maddumCache.rotter, RValue_makeReal(rotter));
        Instance_setSelfVar(inst, maddumCache.rot, RValue_makeReal(rot));
    } else {
        
        int32_t spriteIds[4] = { 296, 295, 294, 293 };
        for (int32_t p = 0; p < 4; p++) {
            GMLReal px = RValue_toReal(selfArrayGet(inst, maddumCache.partx, p));
            GMLReal py = RValue_toReal(selfArrayGet(inst, maddumCache.party, p));
            GMLReal pr = RValue_toReal(selfArrayGet(inst, maddumCache.partrot, p));
            int32_t partSubimg = (p == 3) ? faceemotion : subimg;
            Renderer_drawSpriteExt(r, spriteIds[p], partSubimg,
                                   inst->x + (float)px, inst->y + (float)py,
                                   2.0f, 2.0f, (float)pr, 0xFFFFFFu, 1.0f);
        }
    }

    if (mode == 1) {
        int32_t check = 1;
        GMLReal fakegrav = (maddumCache.fakegrav >= 0) ? selfReal(inst, maddumCache.fakegrav) : 0.0;
        fakegrav += 0.5;
        GMLReal ib2 = 0.0;
        if (maddumCache.gIdealborder >= 0) {
            int64_t k = ((int64_t)maddumCache.gIdealborder << 32) | (uint32_t)2;
            ptrdiff_t p = hmgeti(ctx->globalArrayMap, k);
            if (p >= 0) ib2 = RValue_toReal(ctx->globalArrayMap[p].value);
        }

        int32_t go[4] = {1, 1, 1, 1};
        for (int32_t i = 0; i < 4; i++) {
            GMLReal py = RValue_toReal(selfArrayGet(inst, maddumCache.party, i));
            if ((py + inst->y) < (ib2 - 25.0)) {
                py += fakegrav;
            } else {
                py = ib2 - 20.0 - inst->y;
                check += 1;
                go[i] = 0;
            }
            selfArraySet(inst, maddumCache.party, i, RValue_makeReal(py));
            if (maddumCache.go >= 0) selfArraySet(inst, maddumCache.go, i, RValue_makeReal((GMLReal)go[i]));
        }

        
        static const float partxDrift[4] = { 2.0f, 4.0f, -1.0f, -3.0f };
        static const float partrotDrift[4] = { 2.0f, 5.0f, -3.0f, -9.0f };
        for (int32_t i = 0; i < 4; i++) {
            if (go[i] == 1) {
                GMLReal px = RValue_toReal(selfArrayGet(inst, maddumCache.partx, i));
                GMLReal pr = RValue_toReal(selfArrayGet(inst, maddumCache.partrot, i));
                px += partxDrift[i];
                pr += partrotDrift[i];
                selfArraySet(inst, maddumCache.partx, i, RValue_makeReal(px));
                selfArraySet(inst, maddumCache.partrot, i, RValue_makeReal(pr));
            }
        }
        if (check == 4) {
            Instance_setSelfVar(inst, maddumCache.mode, RValue_makeReal(3.0));
        }
        if (maddumCache.check >= 0) Instance_setSelfVar(inst, maddumCache.check, RValue_makeReal((GMLReal)check));
        if (maddumCache.fakegrav >= 0) Instance_setSelfVar(inst, maddumCache.fakegrav, RValue_makeReal(fakegrav));
    }

    if (mode == 2) {
        GMLReal dingus = (maddumCache.dingus >= 0) ? selfReal(inst, maddumCache.dingus) : 0.0;
        dingus += 1.0;
        
        for (int32_t i = 0; i < 4; i++) {
            GMLReal px = RValue_toReal(selfArrayGet(inst, maddumCache.partx, i));
            GMLReal py = RValue_toReal(selfArrayGet(inst, maddumCache.party, i));
            GMLReal pr = RValue_toReal(selfArrayGet(inst, maddumCache.partrot, i));
            GMLReal opx = (maddumCache.opartx >= 0) ? RValue_toReal(selfArrayGet(inst, maddumCache.opartx, i)) : 0.0;
            GMLReal opy = (maddumCache.oparty >= 0) ? RValue_toReal(selfArrayGet(inst, maddumCache.oparty, i)) : 0.0;
            GMLReal opr = (maddumCache.opartrot >= 0) ? RValue_toReal(selfArrayGet(inst, maddumCache.opartrot, i)) : 0.0;
            px -= (px - opx) / 4.0;
            py -= (py - opy) / 4.0;
            pr -= (pr - opr) / 4.0;
            selfArraySet(inst, maddumCache.partx, i, RValue_makeReal(px));
            selfArraySet(inst, maddumCache.party, i, RValue_makeReal(py));
            selfArraySet(inst, maddumCache.partrot, i, RValue_makeReal(pr));
        }
        if (dingus > 20.0) {
            Instance_setSelfVar(inst, maddumCache.mode, RValue_makeReal(0.0));
            if (maddumCache.check >= 0) Instance_setSelfVar(inst, maddumCache.check, RValue_makeReal(0.0));
            dingus = 0.0;
        }
        if (maddumCache.dingus >= 0) Instance_setSelfVar(inst, maddumCache.dingus, RValue_makeReal(dingus));
    }
}






static struct {
    int32_t splode, alp, i;
    bool ready;
} whitesploderCache = { .ready = false };

static void initWhitesploderCache(DataWin* dw) {
    whitesploderCache.splode = findSelfVarId(dw, "splode");
    whitesploderCache.alp    = findSelfVarId(dw, "alp");
    whitesploderCache.i      = findSelfVarId(dw, "i");
    whitesploderCache.ready  = (whitesploderCache.splode >= 0);
}

static void native_whitesploder_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!whitesploderCache.ready || runner->renderer == NULL || runner->currentRoom == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal splode = selfReal(inst, whitesploderCache.splode) + 1.0;
    Instance_setSelfVar(inst, whitesploderCache.splode, RValue_makeReal(splode));

    float rw = (float)runner->currentRoom->width;
    float rh = (float)runner->currentRoom->height;
    float midY = rh * 0.5f;

    r->drawColor = 0xFFFFFFu;
    for (int32_t i = 0; i < 16; i++) {
        float alp = (float)(splode / 12.0 - 0.06 * (double)i);
        if (alp < 0.0f) alp = 0.0f;
        if (alp > 1.0f) alp = 1.0f;
        r->drawAlpha = alp;
        
        
        drawFilledRect(r, 0.0f, midY - 8.0f * (float)(i + 1), rw, midY - 8.0f * (float)i);
        drawFilledRect(r, 0.0f, midY + 8.0f * (float)(i + 1), rw, midY + 8.0f * (float)i);
    }
    r->drawAlpha = 1.0f;
    
    if (whitesploderCache.i >= 0)
        Instance_setSelfVar(inst, whitesploderCache.i, RValue_makeReal(16.0));
}






static struct {
    int32_t on, amt, siner, reverse;
    bool ready;
} discoballCache = { .ready = false };

static void initDiscoballCache(DataWin* dw) {
    discoballCache.on      = findSelfVarId(dw, "on");
    discoballCache.amt     = findSelfVarId(dw, "amt");
    discoballCache.siner   = findSelfVarId(dw, "siner");
    discoballCache.reverse = findSelfVarId(dw, "reverse");
    discoballCache.ready   = (discoballCache.on >= 0 && discoballCache.amt >= 0 &&
                              discoballCache.siner >= 0);
}














static uint32_t nativeMakeColorHsvBGR(double h255, double s255, double v255) {
    double h = h255 / 255.0 * 360.0;
    double s = s255 / 255.0;
    double v = v255 / 255.0;
    double c = v * s;
    double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    double m = v - c;
    double r1, g1, b1;
    if      (360.0 > h && h >= 300.0) { r1 = c; g1 = 0; b1 = x; }
    else if (300.0 > h && h >= 240.0) { r1 = x; g1 = 0; b1 = c; }
    else if (240.0 > h && h >= 180.0) { r1 = 0; g1 = x; b1 = c; }
    else if (180.0 > h && h >= 120.0) { r1 = 0; g1 = c; b1 = x; }
    else if (120.0 > h && h >=  60.0) { r1 = x; g1 = c; b1 = 0; }
    else                               { r1 = c; g1 = x; b1 = 0; }
    int32_t R = (int32_t)round((r1 + m) * 255.0);
    int32_t G = (int32_t)round((g1 + m) * 255.0);
    int32_t B = (int32_t)round((b1 + m) * 255.0);
    
    
    return (uint32_t)(R | (G << 8) | (B << 16));
}

static void native_discoball_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!discoballCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t on = selfInt(inst, discoballCache.on);
    GMLReal amt = selfReal(inst, discoballCache.amt);
    GMLReal siner = selfReal(inst, discoballCache.siner);
    int32_t reverse = (discoballCache.reverse >= 0) ? selfInt(inst, discoballCache.reverse) : 0;

    
    if (on == 1) {
        if (amt <= 1.0) amt += 0.05;
        if (inst->y < 0.0f) inst->y += 1.0f;
        else on = 0;
    }
    if (on == 2) {
        if (amt > 0.0) amt -= 0.05;
        if (inst->y > inst->ystart) inst->y -= 1.0f;
        else { Runner_destroyInstance(runner, inst); return; }
    }
    if (reverse == 0) siner += 1.0; else siner -= 1.0;

    
    Instance_setSelfVar(inst, discoballCache.on,    RValue_makeReal((GMLReal)on));
    Instance_setSelfVar(inst, discoballCache.amt,   RValue_makeReal(amt));
    Instance_setSelfVar(inst, discoballCache.siner, RValue_makeReal(siner));

    
    int32_t savedPrec = r->circlePrecision;
    r->circlePrecision = 8;
    r->drawAlpha = (float)(0.5 * amt);
    for (int32_t i = 0; i < 12; i++) {
        uint32_t col = nativeMakeColorHsvBGR((double)(i * 20) + siner, 255.0, 255.0);
        r->drawColor = col;
        float cx = inst->x + (float)(sin(((double)(i * 10) + siner) / 20.0) * 40.0);
        float cy = inst->y + (float)(cos(((double)(i * 10) + siner) / 20.0) * 20.0) + 140.0f;
        Renderer_drawCircle(r, cx, cy, 3.0f, false);
    }
    r->drawAlpha = (float)(0.4 * amt);
    for (int32_t i = 0; i < 24; i++) {
        uint32_t col = nativeMakeColorHsvBGR((double)(i * 20) + siner, 255.0, 255.0);
        r->drawColor = col;
        float cx = inst->x + (float)(sin(((double)(i * 10) + siner) / 20.0) * 80.0);
        float cy = inst->y + (float)(cos(((double)(i *  5) + siner) / 20.0) * 60.0) + 140.0f;
        Renderer_drawCircle(r, cx, cy, 6.0f, false);
    }
    r->circlePrecision = savedPrec;
    r->drawAlpha = 1.0f;

    
    Renderer_drawSprite(r, inst->spriteIndex, (int32_t)inst->imageIndex, inst->x, inst->y);
}





static struct {
    int32_t xprev2, yprev2;
    bool ready;
} milkofhellCache = { .ready = false };

static void initMilkofhellCache(DataWin* dw) {
    milkofhellCache.xprev2 = findSelfVarId(dw, "xprev2");
    milkofhellCache.yprev2 = findSelfVarId(dw, "yprev2");
    milkofhellCache.ready  = true; 
}

static void native_milkofhell_shot_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    float xp2 = inst->x - (inst->x - inst->xprevious) * 2.0f;
    float yp2 = inst->y - (inst->y - inst->yprevious) * 2.0f;
    if (milkofhellCache.xprev2 >= 0) Instance_setSelfVar(inst, milkofhellCache.xprev2, RValue_makeReal((GMLReal)xp2));
    if (milkofhellCache.yprev2 >= 0) Instance_setSelfVar(inst, milkofhellCache.yprev2, RValue_makeReal((GMLReal)yp2));

    int32_t spr = inst->spriteIndex;
    int32_t idx = (int32_t)inst->imageIndex;
    float xs = inst->imageXscale, ys = inst->imageYscale;
    float ang = inst->imageAngle;
    Renderer_drawSpriteExt(r, spr, idx, xp2, yp2, xs - 0.6f, ys - 0.6f, ang, 0xFFFFFFu, 0.3f);
    Renderer_drawSpriteExt(r, spr, idx, inst->xprevious, inst->yprevious, xs - 0.3f, ys - 0.3f, ang, 0xFFFFFFu, 0.6f);
    Renderer_drawSpriteExt(r, spr, idx, inst->x, inst->y, xs, ys, ang, 0xFFFFFFu, 1.0f);
}






static struct {
    int32_t voff, write, tx, doom, stringer, doomtimer;
    bool ready;
} mettnewsCache = { .ready = false };

static void initMettnewsCache(DataWin* dw) {
    mettnewsCache.voff      = findSelfVarId(dw, "voff");
    mettnewsCache.write     = findSelfVarId(dw, "write");
    mettnewsCache.tx        = findSelfVarId(dw, "tx");
    mettnewsCache.doom      = findSelfVarId(dw, "doom");
    mettnewsCache.stringer  = findSelfVarId(dw, "stringer");
    mettnewsCache.doomtimer = findSelfVarId(dw, "doomtimer");
    mettnewsCache.ready = (mettnewsCache.voff >= 0 && mettnewsCache.tx >= 0 &&
                           mettnewsCache.stringer >= 0);
}

static void native_mettnews_ticker_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!mettnewsCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal voff = selfReal(inst, mettnewsCache.voff);
    int32_t write = (mettnewsCache.write >= 0) ? selfInt(inst, mettnewsCache.write) : 0;
    GMLReal tx = selfReal(inst, mettnewsCache.tx);
    int32_t doom = (mettnewsCache.doom >= 0) ? selfInt(inst, mettnewsCache.doom) : 0;

    
    r->drawColor = 0x000000u;
    drawFilledRect(r, inst->x, inst->y - 1.0f + (float)voff,
                      inst->x + 330.0f, inst->y + 242.0f + (float)voff);

    if (write == 1) {
        
        
        
        r->drawColor = 0x00FFFFu;
        tx += 1.0;
        if (doom == 1) tx += 4.0;
        nativeSetFont(r, ctx, 2);
        const char* stringer = selfString(inst, mettnewsCache.stringer);
        if (stringer == NULL) stringer = "";
        if (doom == 0) {
            nativeDrawText(runner, r, inst->x + 320.0f - (float)tx, inst->y + 10.0f + (float)voff, stringer);
        } else {
            char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, stringer);
            r->vtable->drawText(r, processed, inst->x + 320.0f - (float)tx,
                                inst->y + 10.0f + (float)voff, 2.0f, 1.0f, 0.0f);
            free(processed);
        }
    }
    
    
    Instance_setSelfVar(inst, mettnewsCache.tx, RValue_makeReal(tx));

    Renderer_drawSprite(r, 1886, 0, inst->x, inst->y);

    if (doom == 1) {
        GMLReal doomtimer = (mettnewsCache.doomtimer >= 0) ? selfReal(inst, mettnewsCache.doomtimer) : 0.0;
        doomtimer += 1.0;
        if (doomtimer > 150.0) {
            
            
            
            if (mettnewsCache.doomtimer >= 0)
                Instance_setSelfVar(inst, mettnewsCache.doomtimer, RValue_makeReal(doomtimer));
            Runner_executeEvent(runner, inst, 7, 11); 
        } else {
            if (mettnewsCache.doomtimer >= 0)
                Instance_setSelfVar(inst, mettnewsCache.doomtimer, RValue_makeReal(doomtimer));
        }
    }

    if (voff > 0.0) voff -= 4.0;
    if (voff <= 0.0) voff = 0.0;
    Instance_setSelfVar(inst, mettnewsCache.voff, RValue_makeReal(voff));

    if (runner->currentRoom != NULL && inst->y > (float)runner->currentRoom->height) {
        Runner_destroyInstance(runner, inst);
    }
}







static struct {
    int32_t basic, siner, i, j, done;
    int32_t objMainchara;
    bool ready;
} kitchenFFCache = { .ready = false };

static void initKitchenForcefieldCache(DataWin* dw) {
    kitchenFFCache.basic = findSelfVarId(dw, "basic");
    kitchenFFCache.siner = findSelfVarId(dw, "siner");
    kitchenFFCache.i     = findSelfVarId(dw, "i");
    kitchenFFCache.j     = findSelfVarId(dw, "j");
    kitchenFFCache.done  = findSelfVarId(dw, "done");
    kitchenFFCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    kitchenFFCache.ready = (kitchenFFCache.siner >= 0);
}

static void native_kitchenForcefield_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!kitchenFFCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t basic = (kitchenFFCache.basic >= 0) ? selfInt(inst, kitchenFFCache.basic) : 0;
    if (basic == 1 && kitchenFFCache.objMainchara >= 0) {
        
        InstanceBBox selfBBox = Collision_computeBBox(ctx->dataWin, inst);
        if (selfBBox.valid) {
            double minDist = 1e10;
            int32_t count = (int32_t)arrlen(runner->instances);
            for (int32_t i = 0; i < count; i++) {
                Instance* other = runner->instances[i];
                if (!other->active || other == inst) continue;
                if (!Collision_matchesTarget(ctx->dataWin, other, kitchenFFCache.objMainchara)) continue;
                InstanceBBox ob = Collision_computeBBox(ctx->dataWin, other);
                if (!ob.valid) continue;
                double xd = 0.0, yd = 0.0;
                if (ob.left  > selfBBox.right)  xd = ob.left  - selfBBox.right;
                if (selfBBox.left > ob.right)   xd = selfBBox.left - ob.right;
                if (ob.top   > selfBBox.bottom) yd = ob.top   - selfBBox.bottom;
                if (selfBBox.top > ob.bottom)   yd = selfBBox.top - ob.bottom;
                double d = sqrt(xd * xd + yd * yd);
                if (d < minDist) minDist = d;
            }
            double cl = minDist;
            if (cl > 40.0) cl = 40.0;
            if (cl < 10.0) cl = 10.0;
            inst->imageAlpha = (float)(1.0 - (cl - 10.0) / 30.0);
        }
    }

    GMLReal siner = selfReal(inst, kitchenFFCache.siner);
    float alpha = inst->imageAlpha;
    int32_t subimg = (int32_t)(siner / 3.0);

    
    if (inst->imageYscale > 1.0f) {
        int32_t limit = (int32_t)inst->imageYscale;
        float frac = inst->imageYscale;
        for (int32_t i = 0; i <= limit; i++) {
            if ((float)i >= frac) break;
            if (i == 0) {
                Renderer_drawSpriteExt(r, 1800, subimg, inst->x, inst->y + (float)(i * 20),
                                       1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
            } else {
                if ((float)(i + 1) >= frac) {
                    Renderer_drawSpriteExt(r, 1800, subimg, inst->x, inst->y + (float)(i * 20) + 20.0f,
                                           1.0f, -1.0f, 0.0f, 0xFFFFFFu, alpha);
                } else {
                    Renderer_drawSpriteExt(r, 1801, subimg, inst->x, inst->y + (float)(i * 20),
                                           1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
                }
            }
        }
    }

    
    if (inst->imageXscale > 1.0f) {
        int32_t limit = (int32_t)inst->imageXscale;
        float frac = inst->imageXscale;
        for (int32_t j = 0; j <= limit; j++) {
            if ((float)j >= frac) break;
            if (j == 0) {
                Renderer_drawSpriteExt(r, 1802, subimg, inst->x + (float)(j * 20), inst->y,
                                       1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
            } else {
                if ((float)(j + 1) >= frac) {
                    Renderer_drawSpriteExt(r, 1802, subimg, inst->x + (float)(j * 20) + 20.0f, inst->y,
                                           -1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
                } else {
                    Renderer_drawSpriteExt(r, 1803, subimg, inst->x + (float)(j * 20) + 20.0f, inst->y,
                                           -1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
                }
            }
        }
    }

    siner += 1.0;
    Instance_setSelfVar(inst, kitchenFFCache.siner, RValue_makeReal(siner));
}






static struct {
    int32_t animimg, arm, xoff, yoff;
    bool ready;
} mettatonDress2Cache = { .ready = false };

static void initMettatonDress2Cache(DataWin* dw) {
    mettatonDress2Cache.animimg = findSelfVarId(dw, "animimg");
    mettatonDress2Cache.arm     = findSelfVarId(dw, "arm");
    mettatonDress2Cache.xoff    = findSelfVarId(dw, "xoff");
    mettatonDress2Cache.yoff    = findSelfVarId(dw, "yoff");
    mettatonDress2Cache.ready   = (mettatonDress2Cache.animimg >= 0 && mettatonDress2Cache.arm >= 0);
}

static void native_mettaton_dress2_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!mettatonDress2Cache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    Renderer_drawSprite(r, inst->spriteIndex, 0, inst->x, inst->y);

    GMLReal animimg = selfReal(inst, mettatonDress2Cache.animimg) + 0.25;
    Instance_setSelfVar(inst, mettatonDress2Cache.animimg, RValue_makeReal(animimg));
    inst->depth = 50000 - (int32_t)(inst->y * 10.0f);

    int32_t arm = selfInt(inst, mettatonDress2Cache.arm);
    float xoff = (mettatonDress2Cache.xoff >= 0) ? (float)selfReal(inst, mettatonDress2Cache.xoff) : 0.0f;
    float yoff = (mettatonDress2Cache.yoff >= 0) ? (float)selfReal(inst, mettatonDress2Cache.yoff) : 0.0f;

    struct { int32_t sprId; float dx; float dy; int32_t fixedSub; } armTbl[13] = {
         { 1814, -19.0f,       -26.0f,       -1 },
         { 1820, -19.0f,       -34.0f,       -1 },
         { 1818, -29.0f,       -36.0f,       -1 },
         { 1815, -19.0f,       -36.0f,       -1 },
         { 1816, -24.0f,       -35.0f,       -1 },
         { 1817, -37.0f,       -31.0f,       -1 },
         { 1819, -20.0f,       -22.0f,       -1 },
         { 1823, -20.0f,       -25.0f,       -1 },
         { 1821, -33.0f,       -37.0f,       -1 },
         { 1822, -36.0f,       -33.0f,       -1 },
         { 1824, -33.0f,       -34.0f,       -1 },
         { 1825, -33.0f,       -37.0f,       -1 },
         { 1826, -33.0f,       -37.0f,        2 }, 
    };
    if (arm >= 0 && arm < 13) {
        int32_t sub = (armTbl[arm].fixedSub >= 0) ? armTbl[arm].fixedSub : (int32_t)animimg;
        Renderer_drawSprite(r, armTbl[arm].sprId, sub,
                            inst->x + armTbl[arm].dx + xoff,
                            inst->y + armTbl[arm].dy + yoff);
    }
}







static struct {
    int32_t bb, cc, dd, on, mega, a, b, c, d;
    bool ready;
} memoryheadCache = { .ready = false };

static void initMemoryheadCache(DataWin* dw) {
    memoryheadCache.bb   = findSelfVarId(dw, "bb");
    memoryheadCache.cc   = findSelfVarId(dw, "cc");
    memoryheadCache.dd   = findSelfVarId(dw, "dd");
    memoryheadCache.on   = findSelfVarId(dw, "on");
    memoryheadCache.mega = findSelfVarId(dw, "mega");
    memoryheadCache.a    = findSelfVarId(dw, "a");
    memoryheadCache.b    = findSelfVarId(dw, "b");
    memoryheadCache.c    = findSelfVarId(dw, "c");
    memoryheadCache.d    = findSelfVarId(dw, "d");
    memoryheadCache.ready = (memoryheadCache.bb >= 0 && memoryheadCache.cc >= 0 &&
                             memoryheadCache.dd >= 0 && memoryheadCache.on >= 0 &&
                             memoryheadCache.a >= 0);
}

static void native_memoryheadBody_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!memoryheadCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    DataWin* dw = ctx->dataWin;

    
    GMLReal bb = selfReal(inst, memoryheadCache.bb);
    GMLReal cc = selfReal(inst, memoryheadCache.cc);
    GMLReal dd = selfReal(inst, memoryheadCache.dd);
    if (bb == 0.0) bb = 1.0;
    if (cc == 0.0) cc = 1.0;
    if (dd == 0.0) dd = 1.0;

    int32_t on = selfInt(inst, memoryheadCache.on);
    if (on == 1) {
        
        
        
        
        
        
        if (memoryheadCache.b >= 0) Instance_setSelfVar(inst, memoryheadCache.b, RValue_makeReal(bb));
        if (memoryheadCache.c >= 0) Instance_setSelfVar(inst, memoryheadCache.c, RValue_makeReal(cc));
        if (memoryheadCache.d >= 0) Instance_setSelfVar(inst, memoryheadCache.d, RValue_makeReal(dd));

        GMLReal a = selfReal(inst, memoryheadCache.a) + 1.0;

        int32_t sprIdx = inst->spriteIndex;
        int32_t subimg = (int32_t)inst->imageIndex;
        if (sprIdx >= 0 && (uint32_t)sprIdx < dw->sprt.count) {
            Sprite* spr = &dw->sprt.sprites[sprIdx];
            int32_t sh = (int32_t)spr->height;
            int32_t sw = (int32_t)spr->width;
            float alpha = inst->imageAlpha;
            for (int32_t i = 0; i < sh; i++) {
                a += 1.0;
                int32_t srcH = (int32_t)(sin(a) * dd);  
                if (srcH <= 0) continue;                
                float dx = (float)inst->x + (float)(sin(a / bb) * cc);
                float dy = (float)inst->y + (float)(i * 2);
                Renderer_drawSpritePartExt(r, sprIdx, subimg, 0, i, sw, srcH,
                                           dx, dy, 2.0f, 2.0f, 0xFFFFFFu, alpha);
            }
        }
        Instance_setSelfVar(inst, memoryheadCache.a, RValue_makeReal(a));
    } else {
        
        Renderer_drawSpriteExt(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                               inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, 1.0f);
    }

    
    Instance_setSelfVar(inst, memoryheadCache.bb, RValue_makeReal(bb));
    Instance_setSelfVar(inst, memoryheadCache.cc, RValue_makeReal(cc));
    Instance_setSelfVar(inst, memoryheadCache.dd, RValue_makeReal(dd));

    
    int32_t mega = (memoryheadCache.mega >= 0) ? selfInt(inst, memoryheadCache.mega) : 0;
    if (mega == 1) {
        cc += 1.0;
        inst->imageAlpha -= 0.03f;
        if (inst->imageAlpha <= 0.0f) mega = 4;
        Instance_setSelfVar(inst, memoryheadCache.cc, RValue_makeReal(cc));
    }
    if (mega == 2) {
        if (cc > 1.0) cc -= 1.0;
        if (inst->imageAlpha < 1.0f) inst->imageAlpha += 0.03f;
        if (cc <= 1.0) {
            mega = 0;
            on   = 0;
            inst->alarm[2] = -1;
            inst->alarm[1] = 90;
            Instance_setSelfVar(inst, memoryheadCache.on, RValue_makeReal(0.0));
        }
        Instance_setSelfVar(inst, memoryheadCache.cc, RValue_makeReal(cc));
    }
    if (memoryheadCache.mega >= 0)
        Instance_setSelfVar(inst, memoryheadCache.mega, RValue_makeReal((GMLReal)mega));
}





static struct {
    int32_t hue;
    bool ready;
} afterimageAsrielCache = { .ready = false };

static void initAfterimageAsrielCache(DataWin* dw) {
    afterimageAsrielCache.hue   = findSelfVarId(dw, "hue");
    afterimageAsrielCache.ready = (afterimageAsrielCache.hue >= 0);
}

static void native_afterimageAsriel_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!afterimageAsrielCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    inst->imageAlpha -= 0.02f;
    GMLReal hue = selfReal(inst, afterimageAsrielCache.hue) + 9.0;
    Instance_setSelfVar(inst, afterimageAsrielCache.hue, RValue_makeReal(hue));

    uint32_t mycolor = nativeMakeColorHsvBGR(hue, 255.0, 250.0);
    Renderer_drawSpriteExt(r, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, 2.0f, 2.0f, 0.0f,
                           mycolor, inst->imageAlpha);

    if (inst->imageAlpha < 0.06f) {
        Runner_destroyInstance(runner, inst);
    }
}






#define WRAPSHOCK_MAX_FACE_VARIDS 16
static struct {
    int32_t oo, freeze, siner, s_timer, shock;
    int32_t ss, ii, mf, type, click, goof;
    
    
    
    
    int32_t faceCandidates[WRAPSHOCK_MAX_FACE_VARIDS];
    int32_t faceCandidateCount;
    bool ready;
} wrapshockCache = { .ready = false };

static void initWrapshockCache(DataWin* dw) {
    wrapshockCache.oo      = findSelfVarId(dw, "oo");
    wrapshockCache.freeze  = findSelfVarId(dw, "freeze");
    wrapshockCache.siner   = findSelfVarId(dw, "siner");
    wrapshockCache.s_timer = findSelfVarId(dw, "s_timer");
    wrapshockCache.shock   = findSelfVarId(dw, "shock");
    wrapshockCache.ss      = findSelfVarId(dw, "ss");
    wrapshockCache.ii      = findSelfVarId(dw, "ii");
    wrapshockCache.mf      = findSelfVarId(dw, "mf");
    wrapshockCache.type    = findSelfVarId(dw, "type");
    wrapshockCache.click   = findSelfVarId(dw, "click");
    wrapshockCache.goof    = findSelfVarId(dw, "goof");
    wrapshockCache.faceCandidateCount = findAllSelfVarIds(dw, "face",
                                                          wrapshockCache.faceCandidates,
                                                          WRAPSHOCK_MAX_FACE_VARIDS);
    wrapshockCache.ready = (wrapshockCache.oo >= 0 && wrapshockCache.siner >= 0 &&
                            wrapshockCache.s_timer >= 0 && wrapshockCache.shock >= 0 &&
                            wrapshockCache.mf >= 0 && wrapshockCache.type >= 0 &&
                            wrapshockCache.faceCandidateCount > 0);
    if (wrapshockCache.faceCandidateCount > 1) {
        fprintf(stderr, "NativeScripts: wrapshock 'face' has %d VARI entries — per-instance resolve enabled\n",
                wrapshockCache.faceCandidateCount);
    }
}


static inline uint32_t nativeMakeColorRgbClamped(int32_t R, int32_t G, int32_t B) {
    if (R < 0) R = 0; if (R > 255) R = 255;
    if (G < 0) G = 0; if (G > 255) G = 255;
    if (B < 0) B = 0; if (B > 255) B = 255;
    return (uint32_t)(R | (G << 8) | (B << 16));
}

static void native_wrapshock_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!wrapshockCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    int32_t oo = selfInt(inst, wrapshockCache.oo);
    oo = (oo == 0) ? 1 : 0;
    Instance_setSelfVar(inst, wrapshockCache.oo, RValue_makeReal((GMLReal)oo));

    int32_t freeze = (wrapshockCache.freeze >= 0) ? selfInt(inst, wrapshockCache.freeze) : 0;
    GMLReal siner = selfReal(inst, wrapshockCache.siner);
    if (freeze == 0) siner += 1.0;

    GMLReal s_timer = selfReal(inst, wrapshockCache.s_timer) + 1.0;
    int32_t shock = selfInt(inst, wrapshockCache.shock);
    if (s_timer > 60.0) shock = 0;

    GMLReal mf = selfReal(inst, wrapshockCache.mf);
    GMLReal ss = sin(siner / 6.0) * 90.0 * mf;
    GMLReal ii_val = -sin(siner / 6.0) * mf;
    if (wrapshockCache.ss >= 0) Instance_setSelfVar(inst, wrapshockCache.ss, RValue_makeReal(ss));
    if (wrapshockCache.ii >= 0) Instance_setSelfVar(inst, wrapshockCache.ii, RValue_makeReal(ii_val));

    
    if (mf < 1.0 && inst->y < 0.0f) inst->y += 1.0f;

    int32_t type = selfInt(inst, wrapshockCache.type);
    
    
    int32_t faceVarId = resolveSelfVarIdForInst(inst, wrapshockCache.faceCandidates,
                                                wrapshockCache.faceCandidateCount);
    int32_t face = (faceVarId >= 0) ? selfInt(inst, faceVarId) : 0;
    float alpha = inst->imageAlpha;

    
    uint32_t blend = inst->imageBlend;
    if (ss < 0.0) {
        int32_t g = 255 + (int32_t)ss;
        int32_t b = 255 + (int32_t)ss;
        blend = nativeMakeColorRgbClamped(255, g, b);
        inst->imageBlend = blend;
    }

    if (type == 0) {
        if (mf < 1.0 && inst->y < -30.0f) inst->y += 1.0f;
        inst->x += (float)(cos(siner / 2.0) * 0.5 * mf);
        inst->y += (float)(sin(siner / 12.0) * 0.8 * mf);
        if (oo == 1)
            Renderer_drawSpriteExt(r, 2419, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, (float)ii_val);
        Renderer_drawSpriteExt(r, 2418, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        if (shock == 1) {
            Renderer_drawSpriteExt(r, 2421, 0, inst->x + 58, inst->y - 52, 2.0f, 2.0f, 0.0f, blend, alpha);
        } else {
            if (face == 0) Renderer_drawSpriteExt(r, 2420, 0, inst->x + 58, inst->y - 52, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 1) Renderer_drawSpriteExt(r, 2422, 0, inst->x + 58, inst->y - 52, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 2) Renderer_drawSpriteExt(r, 2422, 1, inst->x + 58, inst->y - 52, 2.0f, 2.0f, 0.0f, blend, alpha);
        }
    }
    else if (type == 1) {
        inst->x += (float)(cos(siner / 2.0) * 0.5 * mf);
        inst->y += (float)(sin(siner / 12.0) * 0.8 * mf);
        if (oo == 1)
            Renderer_drawSpriteExt(r, 2430, 0, inst->x - 12, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, (float)ii_val);
        Renderer_drawSpriteExt(r, 2427, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        if (shock == 1) {
            Renderer_drawSpriteExt(r, 2428, 0, inst->x + 40, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        } else {
            if (face == 0) Renderer_drawSpriteExt(r, 2429, 0, inst->x + 40, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 1) Renderer_drawSpriteExt(r, 2412, 0, inst->x + 40, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 2) Renderer_drawSpriteExt(r, 2424, 0, inst->x + 40, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        }
    }
    else if (type == 2) {
        inst->x += (float)(cos(siner / 2.0) * 0.5 * mf);
        inst->y += (float)(sin(siner / 12.0) * 0.8 * mf);
        if (oo == 1)
            Renderer_drawSpriteExt(r, 2433, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, (float)ii_val);
        Renderer_drawSpriteExt(r, 2431, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        if (shock == 1) {
            Renderer_drawSpriteExt(r, 2432, 0, inst->x + 60, inst->y - 44, 2.0f, 2.0f, 0.0f, blend, alpha);
        } else {
            if (face == 0) Renderer_drawSpriteExt(r, 2432, 0, inst->x + 60, inst->y - 44, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 1) Renderer_drawSpriteExt(r, 2432, 1, inst->x + 60, inst->y - 44, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 2) Renderer_drawSpriteExt(r, 2424, 0, inst->x + 40, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        }
    }
    else if (type == 3) {
        inst->x += (float)(cos(siner / 2.0) * 0.5 * mf);
        inst->y += (float)(sin(siner / 12.0) * 0.8 * mf);
        GMLReal goof = sin(siner / 5.0) * 10.0;
        if (wrapshockCache.goof >= 0)
            Instance_setSelfVar(inst, wrapshockCache.goof, RValue_makeReal(goof));
        Renderer_drawSpriteExt(r, 301, (int32_t)(siner / 5.0),
                               inst->x - 30, (inst->y - 40.0f) + (float)(goof / 3.0),
                               2.0f, 2.0f, 0.0f, blend, 1.0f);
        if (oo == 1)
            Renderer_drawSpriteExt(r, 2435, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, (float)ii_val);
        Renderer_drawSpriteExt(r, 2434, (int32_t)floor(siner / 5.0),
                               inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        if (shock == 1) {
            Renderer_drawSpriteExt(r, 2436, 0, inst->x + 30, inst->y - 40, 2.0f, 2.0f, 0.0f, blend, alpha);
        } else {
            if (face == 0) Renderer_drawSpriteExt(r, 2437, (int32_t)floor(siner / 5.0),
                                                  inst->x + 30, inst->y - 40, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 1) Renderer_drawSpriteExt(r, 2438, 0, inst->x + 30, inst->y - 40, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 2) Renderer_drawSpriteExt(r, 2424, 0, inst->x + 40, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        }
    }
    else if (type == 4) {
        inst->x += (float)(cos(siner / 2.0) * 0.5 * mf);
        inst->y += (float)(sin(siner / 12.0) * 0.8 * mf);
        if (oo == 1)
            Renderer_drawSpriteExt(r, 2440, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, (float)ii_val);
        Renderer_drawSpriteExt(r, 2439, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        if (shock == 1) {
            Renderer_drawSpriteExt(r, 2441, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        } else {
            if (face == 0) Renderer_drawSpriteExt(r, 2442, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 1) Renderer_drawSpriteExt(r, 2442, 1, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 2) Renderer_drawSpriteExt(r, 2442, 2, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        }
    }
    else if (type == 5) {
        inst->x += (float)(cos(siner / 2.0) * 0.5 * mf);
        inst->y += (float)(sin(siner / 12.0) * 0.8 * mf);
        if (oo == 1)
            Renderer_drawSpriteExt(r, 2426, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, 0xFFFFFFu, (float)ii_val);
        Renderer_drawSpriteExt(r, 2423, 0, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
        if (shock == 1) {
            Renderer_drawSpriteExt(r, 2424, 0, inst->x + 122, inst->y - 32, 2.0f, 2.0f, 0.0f, blend, alpha);
        } else {
            if (face == 0) Renderer_drawSpriteExt(r, 2425, 0, inst->x + 124, inst->y - 32, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 1) Renderer_drawSpriteExt(r, 2425, 1, inst->x + 124, inst->y - 32, 2.0f, 2.0f, 0.0f, blend, alpha);
            if (face == 2) Renderer_drawSpriteExt(r, 2425, 2, inst->x + 124, inst->y - 32, 2.0f, 2.0f, 0.0f, blend, alpha);
        }
    }

    
    

    Instance_setSelfVar(inst, wrapshockCache.siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, wrapshockCache.s_timer, RValue_makeReal(s_timer));
    Instance_setSelfVar(inst, wrapshockCache.shock, RValue_makeReal((GMLReal)shock));
}







static struct {
    int32_t edge, part, w0, h0, wp, hp;
    int32_t lside, rside, side, curx, size, col, color;
    int32_t rotspeed, ftimer, falpha;
    int32_t image;
    int32_t gMnfight, gTurntimer, gMsg;
    int32_t objAsrielBody; 
    bool ready;
} roundedgeCache = { .ready = false };

static void initRoundedgeCache(VMContext* ctx, DataWin* dw) {
    roundedgeCache.edge     = findSelfVarId(dw, "edge");
    roundedgeCache.part     = findSelfVarId(dw, "part");
    roundedgeCache.w0       = findSelfVarId(dw, "w0");
    roundedgeCache.h0       = findSelfVarId(dw, "h0");
    roundedgeCache.wp       = findSelfVarId(dw, "wp");
    roundedgeCache.hp       = findSelfVarId(dw, "hp");
    roundedgeCache.lside    = findSelfVarId(dw, "lside");
    roundedgeCache.rside    = findSelfVarId(dw, "rside");
    roundedgeCache.side     = findSelfVarId(dw, "side");
    roundedgeCache.curx     = findSelfVarId(dw, "curx");
    roundedgeCache.size     = findSelfVarId(dw, "size");
    roundedgeCache.col      = findSelfVarId(dw, "col");
    roundedgeCache.color    = findSelfVarId(dw, "color");
    roundedgeCache.rotspeed = findSelfVarId(dw, "rotspeed");
    roundedgeCache.ftimer   = findSelfVarId(dw, "ftimer");
    roundedgeCache.falpha   = findSelfVarId(dw, "falpha");
    roundedgeCache.image    = findSelfVarId(dw, "image");
    roundedgeCache.gMnfight    = findGlobalVarId(ctx, "mnfight");
    roundedgeCache.gTurntimer  = findGlobalVarId(ctx, "turntimer");
    roundedgeCache.gMsg        = findGlobalVarId(ctx, "msg");
    roundedgeCache.objAsrielBody = findObjectIndex(dw, "obj_asriel_body");
    roundedgeCache.ready = (roundedgeCache.part >= 0 && roundedgeCache.w0 >= 0 &&
                            roundedgeCache.col >= 0 && roundedgeCache.image >= 0 &&
                            roundedgeCache.ftimer >= 0);
}

static void native_roundedge_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!roundedgeCache.ready || runner->renderer == NULL || runner->currentRoom == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal edge = (roundedgeCache.edge >= 0) ? selfReal(inst, roundedgeCache.edge) : 0.0;
    GMLReal part = selfReal(inst, roundedgeCache.part);
    if (edge == 0.0) { edge = 0.1; if (roundedgeCache.edge >= 0) Instance_setSelfVar(inst, roundedgeCache.edge, RValue_makeReal(edge)); }
    if (part == 0.0) { part = 1.0;  Instance_setSelfVar(inst, roundedgeCache.part, RValue_makeReal(part)); }

    GMLReal w0 = selfReal(inst, roundedgeCache.w0);
    GMLReal h0 = (roundedgeCache.h0 >= 0) ? selfReal(inst, roundedgeCache.h0) : 0.0;
    GMLReal wp = w0 / part;
    GMLReal hp = h0 / part;
    Instance_setSelfVar(inst, roundedgeCache.wp, RValue_makeReal(wp));
    if (roundedgeCache.hp >= 0) Instance_setSelfVar(inst, roundedgeCache.hp, RValue_makeReal(hp));

    
    float viewX = (float)runner->currentRoom->views[runner->viewCurrent].viewX;
    float viewW = (float)runner->currentRoom->views[runner->viewCurrent].viewWidth;
    if (roundedgeCache.lside >= 0) Instance_setSelfVar(inst, roundedgeCache.lside, RValue_makeReal((GMLReal)viewX));
    if (roundedgeCache.rside >= 0) Instance_setSelfVar(inst, roundedgeCache.rside, RValue_makeReal((GMLReal)(viewX + viewW)));
    if (roundedgeCache.side  >= 0) Instance_setSelfVar(inst, roundedgeCache.side,  RValue_makeReal(0.0));
    if (roundedgeCache.curx  >= 0) Instance_setSelfVar(inst, roundedgeCache.curx,  RValue_makeReal(0.0));
    if (roundedgeCache.size  >= 0) Instance_setSelfVar(inst, roundedgeCache.size,  RValue_makeReal(1.0));

    GMLReal col = selfReal(inst, roundedgeCache.col) + 1.0;
    if (col > 254.0) col = 0.0;
    Instance_setSelfVar(inst, roundedgeCache.col, RValue_makeReal(col));
    uint32_t color = nativeMakeColorHsvBGR(col, 233.0, 200.0);
    if (roundedgeCache.color >= 0) Instance_setSelfVar(inst, roundedgeCache.color, RValue_makeReal((GMLReal)color));

    
    int32_t image = selfInt(inst, roundedgeCache.image);
    int32_t subimg = (int32_t)inst->imageIndex;
    float alpha = inst->imageAlpha;
    float rw = (float)runner->currentRoom->width;
    float rwHalf = rw * 0.5f;
    int32_t iparts = (int32_t)part;
    for (int32_t i = 0; i < iparts; i++) {
        int32_t srcX  = (int32_t)((wp * (GMLReal)i) + inst->x);
        int32_t srcY  = 0;
        int32_t srcW  = (int32_t)(wp * (GMLReal)i);
        int32_t srcH  = 999;
        float dy      = -((float)wp * (float)i) * 0.5f;
        
        float dxR = (rwHalf + (float)wp * (float)i) - 6.0f;
        Renderer_drawSpritePartExt(r, image, subimg, srcX, srcY, srcW, srcH,
                                   dxR, dy, (float)i, (float)i, color, alpha);
        
        float dxL = (rwHalf - (float)wp * (float)i) + 6.0f;
        Renderer_drawSpritePartExt(r, image, subimg, srcX, srcY, srcW, srcH,
                                   dxL, dy, -(float)i, (float)i, color, alpha);
    }

    
    float rotspeed = (roundedgeCache.rotspeed >= 0) ? (float)selfReal(inst, roundedgeCache.rotspeed) : 0.0f;
    inst->x += rotspeed;
    if (inst->x > 800.0f) inst->x -= 800.0f;
    if (inst->x < 0.0f)   inst->x += 800.0f;

    GMLReal ftimer = selfReal(inst, roundedgeCache.ftimer) + 1.0;
    GMLReal falpha = (roundedgeCache.falpha >= 0) ? selfReal(inst, roundedgeCache.falpha) : 0.0;

    if (ftimer > 630.0 && ftimer < 671.0) {
        falpha += 0.025;
        r->drawAlpha = (float)falpha;
        r->drawColor = 0xFFFFFFu;
        drawFilledRect(r, -10.0f, -10.0f, 999.0f, 999.0f);
    }
    if (ftimer >= 671.0 && ftimer < 685.0) {
        inst->imageAlpha = 0.5f;
        falpha -= 0.1;
        r->drawAlpha = (float)falpha;
        r->drawColor = 0xFFFFFFu;
        drawFilledRect(r, -10.0f, -10.0f, 999.0f, 999.0f);
    }

    if ((int32_t)ftimer == 671) {
        
        if (roundedgeCache.objAsrielBody >= 0) {
            int32_t cnt = (int32_t)arrlen(runner->instances);
            for (int32_t i = 0; i < cnt; i++) {
                Instance* ai = runner->instances[i];
                if (!ai->active || ai->objectIndex != roundedgeCache.objAsrielBody) continue;
                int32_t aligncon = findSelfVarId(ctx->dataWin, "aligncon");
                int32_t specialnormal = findSelfVarId(ctx->dataWin, "specialnormal");
                if (aligncon >= 0)      Instance_setSelfVar(ai, aligncon, RValue_makeReal(4.0));
                if (specialnormal >= 0) Instance_setSelfVar(ai, specialnormal, RValue_makeReal(0.0));
            }
        }
        if (roundedgeCache.gTurntimer >= 0)
            globalSet(ctx, roundedgeCache.gTurntimer, RValue_makeReal(-2.0));

        
        if (roundedgeCache.gMnfight >= 0 && roundedgeCache.gMsg >= 0) {
            GMLReal mnfight = globalReal(ctx, roundedgeCache.gMnfight);
            if (mnfight == 2.0) {
                BuiltinFunc gt = VMBuiltins_find("scr_gettext");
                if (gt) {
                    RValue arg = RValue_makeString("obj_roundedge_135");
                    RValue result = gt(ctx, &arg, 1);
                    RValue_free(&arg);
                    globalArraySet(ctx, roundedgeCache.gMsg, 0, result);
                }
            }
        }
    }

    Instance_setSelfVar(inst, roundedgeCache.ftimer, RValue_makeReal(ftimer));
    if (roundedgeCache.falpha >= 0) Instance_setSelfVar(inst, roundedgeCache.falpha, RValue_makeReal(falpha));

    r->drawAlpha = 1.0f;
}

















static struct {
    
    int32_t transform, stetch, normal, siner, rely, relx;
    int32_t yoff, xoff;
    int32_t armrot_l, armrot_r, torsorot, headrot;
    int32_t specialarm, shrug, arm_alpha, shrug_x;
    int32_t aligncon, altimer, xxx, yyy, xxoff, yyoff, aimage;
    int32_t starcon, bladecon, guncon, gonercon;
    int32_t type, h_mode, armraise;
    int32_t a_xx1, a_yy1, a_x1_add, a_y1_add;
    int32_t a_xx2, a_yy2, a_x2_add, a_y2_add;
    int32_t gen, hl, ws, hg;
    int32_t s_s, specialnormal, sn;
    int32_t headx, heady, n_siner;
    int32_t fullphrase, len;
    int32_t ignore_border;   
    int32_t con;             
    int32_t originalstring;  
    int32_t powersfx, cr;    
    
    int32_t gFaceemotion, gFlag, gLanguage, gMnfight, gMyfight, gBmenuno;
    int32_t gTurntimer, gMonstername, gIdealborder;
    
    int32_t objHeart, objInstawriter, obj744, obj595;
    bool ready;
} asrielBodyCache = { .ready = false };

static void initAsrielBodyCache(VMContext* ctx, DataWin* dw) {
    asrielBodyCache.transform   = findSelfVarId(dw, "transform");
    asrielBodyCache.stetch      = findSelfVarId(dw, "stetch");
    asrielBodyCache.normal      = findSelfVarId(dw, "normal");
    asrielBodyCache.siner       = findSelfVarId(dw, "siner");
    asrielBodyCache.rely        = findSelfVarId(dw, "rely");
    asrielBodyCache.relx        = findSelfVarId(dw, "relx");
    asrielBodyCache.yoff        = findSelfVarId(dw, "yoff");
    asrielBodyCache.xoff        = findSelfVarId(dw, "xoff");
    asrielBodyCache.armrot_l    = findSelfVarId(dw, "armrot_l");
    asrielBodyCache.armrot_r    = findSelfVarId(dw, "armrot_r");
    asrielBodyCache.torsorot    = findSelfVarId(dw, "torsorot");
    asrielBodyCache.headrot     = findSelfVarId(dw, "headrot");
    asrielBodyCache.specialarm  = findSelfVarId(dw, "specialarm");
    asrielBodyCache.shrug       = findSelfVarId(dw, "shrug");
    asrielBodyCache.arm_alpha   = findSelfVarId(dw, "arm_alpha");
    asrielBodyCache.shrug_x     = findSelfVarId(dw, "shrug_x");
    asrielBodyCache.aligncon    = findSelfVarId(dw, "aligncon");
    asrielBodyCache.altimer     = findSelfVarId(dw, "altimer");
    asrielBodyCache.xxx         = findSelfVarId(dw, "xxx");
    asrielBodyCache.yyy         = findSelfVarId(dw, "yyy");
    asrielBodyCache.xxoff       = findSelfVarId(dw, "xxoff");
    asrielBodyCache.yyoff       = findSelfVarId(dw, "yyoff");
    asrielBodyCache.aimage      = findSelfVarId(dw, "aimage");
    asrielBodyCache.starcon     = findSelfVarId(dw, "starcon");
    asrielBodyCache.bladecon    = findSelfVarId(dw, "bladecon");
    asrielBodyCache.guncon      = findSelfVarId(dw, "guncon");
    asrielBodyCache.gonercon    = findSelfVarId(dw, "gonercon");
    asrielBodyCache.type        = findSelfVarId(dw, "type");
    asrielBodyCache.h_mode      = findSelfVarId(dw, "h_mode");
    asrielBodyCache.armraise    = findSelfVarId(dw, "armraise");
    asrielBodyCache.a_xx1       = findSelfVarId(dw, "a_xx1");
    asrielBodyCache.a_yy1       = findSelfVarId(dw, "a_yy1");
    asrielBodyCache.a_x1_add    = findSelfVarId(dw, "a_x1_add");
    asrielBodyCache.a_y1_add    = findSelfVarId(dw, "a_y1_add");
    asrielBodyCache.a_xx2       = findSelfVarId(dw, "a_xx2");
    asrielBodyCache.a_yy2       = findSelfVarId(dw, "a_yy2");
    asrielBodyCache.a_x2_add    = findSelfVarId(dw, "a_x2_add");
    asrielBodyCache.a_y2_add    = findSelfVarId(dw, "a_y2_add");
    asrielBodyCache.gen         = findSelfVarId(dw, "gen");
    asrielBodyCache.hl          = findSelfVarId(dw, "hl");
    asrielBodyCache.ws          = findSelfVarId(dw, "ws");
    asrielBodyCache.hg          = findSelfVarId(dw, "hg");
    asrielBodyCache.s_s         = findSelfVarId(dw, "s_s");
    asrielBodyCache.specialnormal = findSelfVarId(dw, "specialnormal");
    asrielBodyCache.sn          = findSelfVarId(dw, "sn");
    asrielBodyCache.headx       = findSelfVarId(dw, "headx");
    asrielBodyCache.heady       = findSelfVarId(dw, "heady");
    asrielBodyCache.n_siner     = findSelfVarId(dw, "n_siner");
    asrielBodyCache.fullphrase  = findSelfVarId(dw, "fullphrase");
    asrielBodyCache.len         = findSelfVarId(dw, "len");
    asrielBodyCache.ignore_border = findSelfVarId(dw, "ignore_border");
    asrielBodyCache.con         = findSelfVarId(dw, "con");
    asrielBodyCache.originalstring = findSelfVarId(dw, "originalstring");
    asrielBodyCache.powersfx    = findSelfVarId(dw, "powersfx");
    asrielBodyCache.cr          = findSelfVarId(dw, "cr");

    asrielBodyCache.gFaceemotion  = findGlobalVarId(ctx, "faceemotion");
    asrielBodyCache.gFlag         = findGlobalVarId(ctx, "flag");
    asrielBodyCache.gLanguage     = findGlobalVarId(ctx, "language");
    asrielBodyCache.gMnfight      = findGlobalVarId(ctx, "mnfight");
    asrielBodyCache.gMyfight      = findGlobalVarId(ctx, "myfight");
    asrielBodyCache.gBmenuno      = findGlobalVarId(ctx, "bmenuno");
    asrielBodyCache.gTurntimer    = findGlobalVarId(ctx, "turntimer");
    asrielBodyCache.gMonstername  = findGlobalVarId(ctx, "monstername");
    asrielBodyCache.gIdealborder  = findGlobalVarId(ctx, "idealborder");

    asrielBodyCache.objHeart       = findObjectIndex(dw, "obj_heart");
    asrielBodyCache.objInstawriter = 787;  
    asrielBodyCache.obj744         = 744;
    asrielBodyCache.obj595         = 595;

    asrielBodyCache.ready = (asrielBodyCache.siner >= 0 && asrielBodyCache.rely >= 0 &&
                             asrielBodyCache.normal >= 0 && asrielBodyCache.aligncon >= 0 &&
                             asrielBodyCache.starcon >= 0 && asrielBodyCache.type >= 0);
}


static Instance* asriel_findInstanceById(Runner* runner, int32_t id) {
    int32_t n = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < n; i++) {
        Instance* it = runner->instances[i];
        if (it->active && (int32_t)it->instanceId == id) return it;
    }
    return NULL;
}


static void asriel_withObjectSetVar(Runner* runner, int32_t objIdx, int32_t varId, RValue val) {
    if (objIdx < 0 || varId < 0) { RValue_free(&val); return; }
    int32_t n = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < n; i++) {
        Instance* it = runner->instances[i];
        if (it->active && it->objectIndex == objIdx) {
            Instance_setSelfVar(it, varId, val);
        }
    }
    RValue_free(&val);
}






static GMLReal asriel_casterPlay(VMContext* ctx, GMLReal snd, GMLReal gain, GMLReal pitch) {
    GMLReal inst_id = 0.0;
    BuiltinFunc aps = VMBuiltins_find("audio_play_sound");
    if (aps) {
        RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(100.0), RValue_makeReal(0.0) };
        RValue r = aps(ctx, a, 3);
        inst_id = RValue_toReal(r);
        RValue_free(&r);
    }
    BuiltinFunc asp = VMBuiltins_find("audio_sound_pitch");
    if (asp) {
        RValue a[2] = { RValue_makeReal(snd), RValue_makeReal(pitch) };
        RValue r = asp(ctx, a, 2); RValue_free(&r);
    }
    BuiltinFunc asg = VMBuiltins_find("audio_sound_gain");
    if (asg) {
        RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(gain), RValue_makeReal(0.0) };
        RValue r = asg(ctx, a, 3); RValue_free(&r);
    }
    return inst_id;
}


static void asriel_withObjectDestroy(Runner* runner, int32_t objIdx) {
    if (objIdx < 0) return;
    int32_t n = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < n; i++) {
        Instance* it = runner->instances[i];
        if (it->active && it->objectIndex == objIdx) {
            Runner_destroyInstance(runner, it);
        }
    }
}

static void native_asrielBody_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!asrielBodyCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    DataWin* dw = ctx->dataWin; (void)dw;
    #define C (&asrielBodyCache)

    
    GMLReal stetch = selfReal(inst, C->stetch);
    int32_t transform = selfInt(inst, C->transform);
    if (transform == 1) {
        stetch += 0.2;
        Instance_setSelfVar(inst, C->stetch, RValue_makeReal(stetch));
    }

    
    int32_t normal = selfInt(inst, C->normal);
    GMLReal siner = selfReal(inst, C->siner);
    GMLReal rely  = selfReal(inst, C->rely);
    if (normal == 1) {
        siner += 1.0;
        rely += sin(siner / 12.0);
        inst->x += (float)(cos(siner / 24.0) * 6.0);
        inst->y += (float)(sin(siner / 6.0) * 0.25);
    }
    GMLReal yoff = sin(siner / 6.0);
    GMLReal xoff = cos(siner / 3.0);
    if (C->yoff >= 0) Instance_setSelfVar(inst, C->yoff, RValue_makeReal(yoff));
    if (C->xoff >= 0) Instance_setSelfVar(inst, C->xoff, RValue_makeReal(xoff));

    
    r->drawColor = 0x000000u;
    r->drawAlpha = inst->imageAlpha;
    drawFilledRect(r, inst->x - 40.0f, inst->y + 20.0f + (float)rely,
                      inst->x + 42.0f, inst->y + 46.0f + (float)rely);
    r->drawAlpha = 1.0f;

    
    float px = inst->x;
    float py = inst->y;
    uint32_t blend = inst->imageBlend;
    float alpha = inst->imageAlpha;
    float scaleX = 2.0f + (float)stetch;
    GMLReal torsorot = (C->torsorot >= 0) ? selfReal(inst, C->torsorot) : 0.0;
    GMLReal armrot_l = (C->armrot_l >= 0) ? selfReal(inst, C->armrot_l) : 0.0;
    GMLReal armrot_r = (C->armrot_r >= 0) ? selfReal(inst, C->armrot_r) : 0.0;
    GMLReal headrot  = (C->headrot  >= 0) ? selfReal(inst, C->headrot)  : 0.0;
    int32_t specialarm = (C->specialarm >= 0) ? selfInt(inst, C->specialarm) : 0;
    int32_t shrug      = (C->shrug      >= 0) ? selfInt(inst, C->shrug)      : 0;
    GMLReal arm_alpha  = (C->arm_alpha  >= 0) ? selfReal(inst, C->arm_alpha) : 1.0;
    GMLReal armraise  = (C->armraise >= 0) ? selfReal(inst, C->armraise) : 0.0;
    GMLReal headx     = (C->headx    >= 0) ? selfReal(inst, C->headx)    : 0.0;
    GMLReal heady     = (C->heady    >= 0) ? selfReal(inst, C->heady)    : 0.0;

    
    Renderer_drawSpriteExt(r, 2464, 0, px + (float)(yoff * 2.0), ((py + 168.0f) - 112.0f) + (float)(rely * 0.9),
                           scaleX, 2.0f, (float)torsorot, blend, alpha);
    Renderer_drawSpriteExt(r, 2463, 0, px + (float)yoff, py + 48.0f + (float)rely,
                           scaleX, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2462, 0, px + (float)yoff, py + 48.0f + (float)rely,
                           scaleX, 2.0f, (float)torsorot, blend, alpha);
    Renderer_drawSpriteExt(r, 2460, 0, px + 2.0f, py + 34.0f + (float)(rely * 1.2),
                           scaleX, 2.0f, 0.0f, blend, alpha);

    
    float armY = py + 38.0f + (float)(rely * 1.2);
    if (specialarm == 0) {
        if (shrug == 0) {
            Renderer_drawSpriteExt(r, 2458, 0, px - 28.0f, armY, -scaleX, 2.0f, (float)armrot_l, blend, alpha);
            Renderer_drawSpriteExt(r, 2458, 0, px + 30.0f, armY,  scaleX, 2.0f, (float)armrot_r, blend, alpha);
        } else {
            
            Renderer_drawSpriteExt(r, 2459, 0, px - 28.0f, armY, -2.0f, 2.0f, (float)armrot_l, blend, alpha);
            Renderer_drawSpriteExt(r, 2459, 0, px + 30.0f, armY,  2.0f, 2.0f, (float)armrot_r, blend, alpha);
        }
    } else if (specialarm == 1) {
        Renderer_drawSpriteExt(r, 2458, 0, px - 28.0f, armY, -2.0f, 2.0f, (float)armrot_l, blend, (float)arm_alpha);
        Renderer_drawSpriteExt(r, 2458, 0, px + 30.0f, armY,  2.0f, 2.0f, (float)armrot_r, blend, (float)arm_alpha);
    } else if (specialarm == 2) {
        Renderer_drawSpriteExt(r, 2458, 0, px - 28.0f, armY, -2.0f, 2.0f, (float)armrot_l, blend, alpha);
        Renderer_drawSpriteExt(r, 2458, 0, px + 30.0f, armY,  2.0f, 2.0f, (float)armrot_r, blend, (float)arm_alpha);
    }

    
    float shldY = py + 26.0f + (float)(rely * 1.2);
    Renderer_drawSpriteExt(r, 2461, 0, px - 28.0f, shldY, -scaleX, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2461, 0, px + 30.0f, shldY,  scaleX, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2465, 0, px, py + 22.0f + (float)rely, scaleX, 2.0f, 0.0f, blend, alpha);

    
    if (shrug == 0) {
        int32_t face = (C->gFaceemotion >= 0) ? (int32_t)globalReal(ctx, C->gFaceemotion) : 0;
        Renderer_drawSpriteExt(r, 2466, face, px + (float)headx, py + (float)(rely * 1.2) + (float)heady,
                               scaleX, 2.0f, (float)headrot, blend, alpha);
    }
    if (shrug == 1) {
        GMLReal shrug_x = (C->shrug_x >= 0) ? selfReal(inst, C->shrug_x) : 0.0;
        shrug_x += 1.0;
        if (C->shrug_x >= 0) Instance_setSelfVar(inst, C->shrug_x, RValue_makeReal(shrug_x));
        Renderer_drawSpriteExt(r, 2468, (int32_t)floor(shrug_x / 6.0),
                               px + (float)headx, py + (float)(rely * 1.2) + (float)heady,
                               scaleX, 2.0f, (float)headrot, blend, alpha);
    }

    
    int32_t aligncon = selfInt(inst, C->aligncon);
    GMLReal relx = (C->relx >= 0) ? selfReal(inst, C->relx) : 0.0;
    GMLReal xxoff = (C->xxoff >= 0) ? selfReal(inst, C->xxoff) : 0.0;
    GMLReal yyoff = (C->yyoff >= 0) ? selfReal(inst, C->yyoff) : 0.0;
    GMLReal altimer = (C->altimer >= 0) ? selfReal(inst, C->altimer) : 0.0;

    if (aligncon == 1) {
        
        GMLReal xxx = 320.0, yyy = 45.0;
        int32_t s_s = (C->s_s >= 0) ? selfInt(inst, C->s_s) : 0;
        if (s_s == 1) yyy = 100.0;
        if (C->xxx >= 0) Instance_setSelfVar(inst, C->xxx, RValue_makeReal(xxx));
        if (C->yyy >= 0) Instance_setSelfVar(inst, C->yyy, RValue_makeReal(yyy));
        xxoff = (GMLReal)inst->x - xxx;
        yyoff = (GMLReal)inst->y - yyy;
        aligncon = 2;
        altimer = 0;
        normal  = 0;
        Instance_setSelfVar(inst, C->normal, RValue_makeReal(0.0));
    }

    if (aligncon == 2) {
        inst->imageAlpha = 1.0f;
        
        #define DECAY(v) do { if (fabs(v) > 1.0) v *= 0.7; else v = 0.0; } while(0)
        DECAY(relx); DECAY(rely); DECAY(yyoff); DECAY(xxoff);
        DECAY(armrot_l); DECAY(armrot_r); DECAY(torsorot); DECAY(headrot);
        #undef DECAY
        altimer += 1.0;
        GMLReal xxx = (C->xxx >= 0) ? selfReal(inst, C->xxx) : 320.0;
        GMLReal yyy = (C->yyy >= 0) ? selfReal(inst, C->yyy) : 45.0;
        inst->x = (float)(xxx + xxoff);
        inst->y = (float)(yyy + yyoff);
        if (altimer > 15.0) {
            inst->imageAlpha = 1.0f;
            aligncon = 3;
            if (C->aimage >= 0) Instance_setSelfVar(inst, C->aimage, RValue_makeReal(0.0));
        }
    }

    if (aligncon == 4) {
        if (C->heady >= 0)        Instance_setSelfVar(inst, C->heady,     RValue_makeReal(0.0)); heady = 0;
        if (C->headx >= 0)        Instance_setSelfVar(inst, C->headx,     RValue_makeReal(0.0)); headx = 0;
        if (C->specialarm >= 0)   Instance_setSelfVar(inst, C->specialarm,RValue_makeReal(0.0));
        if (C->arm_alpha >= 0)    Instance_setSelfVar(inst, C->arm_alpha, RValue_makeReal(0.0));
        relx = rely = xxoff = yyoff = 0.0;
        armrot_l = armrot_r = torsorot = headrot = 0.0;
        aligncon = 0;
        siner = 0.0;
        if (C->aimage >= 0) Instance_setSelfVar(inst, C->aimage, RValue_makeReal(1.0));
        normal = 1;
        altimer = 0.0;
        Instance_setSelfVar(inst, C->normal, RValue_makeReal(1.0));
    }

    
    if (C->relx  >= 0) Instance_setSelfVar(inst, C->relx,  RValue_makeReal(relx));
    if (C->xxoff >= 0) Instance_setSelfVar(inst, C->xxoff, RValue_makeReal(xxoff));
    if (C->yyoff >= 0) Instance_setSelfVar(inst, C->yyoff, RValue_makeReal(yyoff));
    if (C->altimer >= 0) Instance_setSelfVar(inst, C->altimer, RValue_makeReal(altimer));
    if (C->armrot_l >= 0) Instance_setSelfVar(inst, C->armrot_l, RValue_makeReal(armrot_l));
    if (C->armrot_r >= 0) Instance_setSelfVar(inst, C->armrot_r, RValue_makeReal(armrot_r));
    if (C->torsorot >= 0) Instance_setSelfVar(inst, C->torsorot, RValue_makeReal(torsorot));
    if (C->headrot  >= 0) Instance_setSelfVar(inst, C->headrot,  RValue_makeReal(headrot));
    Instance_setSelfVar(inst, C->aligncon, RValue_makeReal((GMLReal)aligncon));

    

    
    int32_t starcon = selfInt(inst, C->starcon);
    int32_t type    = (C->type >= 0) ? selfInt(inst, C->type) : 0;
    GMLReal h_mode  = (C->h_mode >= 0) ? selfReal(inst, C->h_mode) : 0.0;
    GMLReal gen_id  = (C->gen >= 0) ? selfReal(inst, C->gen) : 0.0;

    if (starcon > 0) {
        if (starcon == 1) {
            if (C->gFaceemotion >= 0) globalSet(ctx, C->gFaceemotion, RValue_makeReal(2.0));
            if (C->powersfx >= 0) {
                GMLReal snd = selfReal(inst, C->powersfx);
                asriel_casterPlay(ctx, snd, 0.8, 1.0);
            }
            armraise = 20.0;
            starcon = 2;
            inst->alarm[5] = 1;
        }
        if (starcon == 3) {
            starcon = 4;
            inst->alarm[5] = 1;
        }
        if (starcon == 5) {
            armrot_l -= armraise;
            armrot_r += armraise;
            armraise -= 2.0;
            if (armraise <= 0.0) {
                starcon = 6;
                inst->alarm[5] = 20;
            }
            if (C->armrot_l >= 0) Instance_setSelfVar(inst, C->armrot_l, RValue_makeReal(armrot_l));
            if (C->armrot_r >= 0) Instance_setSelfVar(inst, C->armrot_r, RValue_makeReal(armrot_r));
        }
        if (starcon >= 5 && starcon <= 9) {
            GMLReal a_xx1 = (GMLReal)inst->x - 28.0;
            GMLReal a_yy1 = (GMLReal)inst->y + 38.0 + rely * 1.2;
            GMLReal a_x1_add_v = 90.0 * cos((armrot_l - 90.0 - 15.0) * (M_PI / 180.0));
            GMLReal a_y1_add_v = -90.0 * sin((armrot_l - 90.0 - 15.0) * (M_PI / 180.0));
            for (int32_t k = 0; k < 2; k++) {
                Instance* hl = Runner_createInstance(runner, a_xx1 + a_x1_add_v, a_yy1 + a_y1_add_v, 573);
                if (hl) {
                    hl->depth = inst->depth + 1;
                    if (C->type >= 0) Instance_setSelfVar(hl, C->type, RValue_makeReal((GMLReal)type));
                    if (C->hl >= 0) Instance_setSelfVar(inst, C->hl, RValue_makeReal((GMLReal)hl->instanceId));
                }
            }
            GMLReal a_xx2 = (GMLReal)inst->x + 30.0;
            GMLReal a_yy2 = (GMLReal)inst->y + 38.0 + rely * 1.2;
            GMLReal a_x2_add_v = 90.0 * cos((armrot_r - 90.0 + 15.0) * (M_PI / 180.0));
            GMLReal a_y2_add_v = -90.0 * sin((armrot_r - 90.0 + 15.0) * (M_PI / 180.0));
            for (int32_t k = 0; k < 2; k++) {
                Instance* hl = Runner_createInstance(runner, a_xx2 + a_x2_add_v, a_yy2 + a_y2_add_v, 573);
                if (hl) {
                    hl->depth = inst->depth + 1;
                    if (C->type >= 0) Instance_setSelfVar(hl, C->type, RValue_makeReal((GMLReal)type));
                    if (C->hl >= 0) Instance_setSelfVar(inst, C->hl, RValue_makeReal((GMLReal)hl->instanceId));
                }
            }
            if (C->a_xx1 >= 0) Instance_setSelfVar(inst, C->a_xx1, RValue_makeReal(a_xx1));
            if (C->a_yy1 >= 0) Instance_setSelfVar(inst, C->a_yy1, RValue_makeReal(a_yy1));
            if (C->a_x1_add >= 0) Instance_setSelfVar(inst, C->a_x1_add, RValue_makeReal(a_x1_add_v));
            if (C->a_y1_add >= 0) Instance_setSelfVar(inst, C->a_y1_add, RValue_makeReal(a_y1_add_v));
            if (C->a_xx2 >= 0) Instance_setSelfVar(inst, C->a_xx2, RValue_makeReal(a_xx2));
            if (C->a_yy2 >= 0) Instance_setSelfVar(inst, C->a_yy2, RValue_makeReal(a_yy2));
            if (C->a_x2_add >= 0) Instance_setSelfVar(inst, C->a_x2_add, RValue_makeReal(a_x2_add_v));
            if (C->a_y2_add >= 0) Instance_setSelfVar(inst, C->a_y2_add, RValue_makeReal(a_y2_add_v));
        }
        if (starcon == 7) {
            starcon = 8;
            inst->alarm[5] = 15;
        }
        if (starcon == 9) starcon = 12;
        if (starcon == 12) {
            Instance* gen_inst = NULL;
            if (type == 0) gen_inst = Runner_createInstance(runner, 0.0, 0.0, 582);
            if (type == 1) gen_inst = Runner_createInstance(runner, 0.0, 0.0, 588);
            if (gen_inst) {
                if (C->h_mode >= 0) Instance_setSelfVar(gen_inst, C->h_mode, RValue_makeReal(h_mode));
                if (C->gen >= 0) {
                    gen_id = (GMLReal)gen_inst->instanceId;
                    Instance_setSelfVar(inst, C->gen, RValue_makeReal(gen_id));
                }
            }
            starcon = 13;
            inst->alarm[5] = 300;
            if (type == 1) inst->alarm[5] = 180;
        }
        if (starcon == 13) {
            if (inst->imageAlpha > 0.0f) inst->imageAlpha -= 0.05f;
        }
        if (starcon == 14) {
            if (C->gFaceemotion >= 0) globalSet(ctx, C->gFaceemotion, RValue_makeReal(0.0));
            
            Instance* gen_inst = asriel_findInstanceById(runner, (int32_t)gen_id);
            if (gen_inst) Runner_destroyInstance(runner, gen_inst);
            armrot_l = 0.0; armrot_r = 0.0;
            inst->imageAlpha += 0.05f;
            if (inst->imageAlpha >= 1.0f) {
                Runner_executeEvent(runner, inst, 7, 11); 
                aligncon = 4;
                starcon = 0;
            }
            if (C->armrot_l >= 0) Instance_setSelfVar(inst, C->armrot_l, RValue_makeReal(armrot_l));
            if (C->armrot_r >= 0) Instance_setSelfVar(inst, C->armrot_r, RValue_makeReal(armrot_r));
            Instance_setSelfVar(inst, C->aligncon, RValue_makeReal((GMLReal)aligncon));
        }
        Instance_setSelfVar(inst, C->starcon, RValue_makeReal((GMLReal)starcon));
        if (C->armraise >= 0) Instance_setSelfVar(inst, C->armraise, RValue_makeReal(armraise));
    }

    
    int32_t bladecon = (C->bladecon >= 0) ? selfInt(inst, C->bladecon) : 0;
    if (bladecon > 0) {
        if (bladecon == 1) {
            armraise = 20.0;
            bladecon = 2;
            if (C->specialarm >= 0) Instance_setSelfVar(inst, C->specialarm, RValue_makeReal(1.0));
            inst->alarm[6] = 30;
        }
        if (bladecon == 2) {
            if (arm_alpha > 0.0) arm_alpha -= 0.05;
        }
        if (bladecon == 3) {
            Instance* gen_inst = Runner_createInstance(runner, (GMLReal)inst->x, (GMLReal)inst->y, 591);
            if (gen_inst) {
                if (C->h_mode >= 0) Instance_setSelfVar(gen_inst, C->h_mode, RValue_makeReal(h_mode));
                if (C->gen >= 0) Instance_setSelfVar(inst, C->gen, RValue_makeReal((GMLReal)gen_inst->instanceId));
            }
            bladecon = 4;
            inst->alarm[6] = 30;
        }
        if (bladecon == 10) {
            heady = 0.0;
            headrot = 0.0;
            if (C->specialarm >= 0) Instance_setSelfVar(inst, C->specialarm, RValue_makeReal(1.0));
            arm_alpha = 0.0;
            bladecon = 11;
            if (C->heady >= 0)   Instance_setSelfVar(inst, C->heady, RValue_makeReal(0.0));
            if (C->headrot >= 0) Instance_setSelfVar(inst, C->headrot, RValue_makeReal(0.0));
        }
        if (bladecon == 11) {
            inst->imageAlpha = 0.0f;
            if (C->heady >= 0)      Instance_setSelfVar(inst, C->heady, RValue_makeReal(0.0));
            if (C->headx >= 0)      Instance_setSelfVar(inst, C->headx, RValue_makeReal(0.0));
            if (C->specialarm >= 0) Instance_setSelfVar(inst, C->specialarm, RValue_makeReal(0.0));
            arm_alpha = 1.0;
            inst->x = 320.0f; inst->y = 50.0f;
            relx = rely = xxoff = yyoff = 0.0;
            armrot_l = armrot_r = torsorot = headrot = 0.0;
            siner = 0.0; altimer = 0.0;
            bladecon = 12;
            if (C->relx >= 0)  Instance_setSelfVar(inst, C->relx, RValue_makeReal(0.0));
            if (C->xxoff >= 0) Instance_setSelfVar(inst, C->xxoff, RValue_makeReal(0.0));
            if (C->yyoff >= 0) Instance_setSelfVar(inst, C->yyoff, RValue_makeReal(0.0));
            if (C->armrot_l >= 0) Instance_setSelfVar(inst, C->armrot_l, RValue_makeReal(0.0));
            if (C->armrot_r >= 0) Instance_setSelfVar(inst, C->armrot_r, RValue_makeReal(0.0));
            if (C->torsorot >= 0) Instance_setSelfVar(inst, C->torsorot, RValue_makeReal(0.0));
            if (C->headrot >= 0)  Instance_setSelfVar(inst, C->headrot, RValue_makeReal(0.0));
            if (C->altimer >= 0)  Instance_setSelfVar(inst, C->altimer, RValue_makeReal(0.0));
        }
        if (bladecon == 12) {
            siner = 0.0;
            inst->imageAlpha += 0.05f;
            if (inst->imageAlpha >= 1.0f) {
                Runner_executeEvent(runner, inst, 7, 11); 
                if (C->specialarm >= 0) Instance_setSelfVar(inst, C->specialarm, RValue_makeReal(0.0));
                aligncon = 4;
                bladecon = 0;
                Instance_setSelfVar(inst, C->aligncon, RValue_makeReal(4.0));
            }
        }
        if (C->bladecon >= 0) Instance_setSelfVar(inst, C->bladecon, RValue_makeReal((GMLReal)bladecon));
        if (C->arm_alpha >= 0) Instance_setSelfVar(inst, C->arm_alpha, RValue_makeReal(arm_alpha));
    }

    
    int32_t guncon = (C->guncon >= 0) ? selfInt(inst, C->guncon) : 0;
    if (guncon > 0) {
        if (guncon == 1) {
            arm_alpha = 1.0;
            guncon = 2;
            if (C->specialarm >= 0) Instance_setSelfVar(inst, C->specialarm, RValue_makeReal(2.0));
            inst->alarm[7] = 20;
        }
        if (guncon == 2) {
            if (arm_alpha > 0.0) arm_alpha -= 0.05;
        }
        if (guncon == 3) {
            Instance* gen_inst = Runner_createInstance(runner, (GMLReal)inst->x + 70.0, (GMLReal)inst->y + 15.0, 585);
            if (gen_inst) {
                if (C->h_mode >= 0) Instance_setSelfVar(gen_inst, C->h_mode, RValue_makeReal(h_mode));
                if (C->gen >= 0) Instance_setSelfVar(inst, C->gen, RValue_makeReal((GMLReal)gen_inst->instanceId));
            }
            guncon = 4;
            inst->alarm[7] = 30;
        }
        if (guncon == 7) {
            arm_alpha += 0.1;
            if (arm_alpha >= 1.0) guncon = 8;
        }
        if (guncon == 8) {
            aligncon = 1;
            guncon = 9;
            inst->alarm[7] = 10;
            Instance_setSelfVar(inst, C->aligncon, RValue_makeReal(1.0));
        }
        if (guncon == 10) {
            Runner_executeEvent(runner, inst, 7, 11); 
            aligncon = 4;
            guncon = 0;
            Instance_setSelfVar(inst, C->aligncon, RValue_makeReal(4.0));
        }
        if (C->guncon >= 0) Instance_setSelfVar(inst, C->guncon, RValue_makeReal((GMLReal)guncon));
        if (C->arm_alpha >= 0) Instance_setSelfVar(inst, C->arm_alpha, RValue_makeReal(arm_alpha));
    }

    
    int32_t gonercon = (C->gonercon >= 0) ? selfInt(inst, C->gonercon) : 0;
    GMLReal ws_id = (C->ws >= 0) ? selfReal(inst, C->ws) : 0.0;
    if (gonercon > 0) {
        if (gonercon == 1) {
            if (C->gFlag >= 0) globalArraySet(ctx, C->gFlag, 20, RValue_makeReal(1.0));
            gonercon = 2;
            inst->alarm[8] = 1;
        }
        if (gonercon == 3) {
            gonercon = 4;
            inst->alarm[8] = 30;
        }
        if (gonercon == 5) {
            
            asriel_withObjectSetVar(runner, C->obj744, C->ignore_border, RValue_makeReal(1.0));
            Instance* ws_inst = Runner_createInstance(runner, 0.0, 0.0, 594);
            if (ws_inst && C->ws >= 0) {
                ws_id = (GMLReal)ws_inst->instanceId;
                Instance_setSelfVar(inst, C->ws, RValue_makeReal(ws_id));
            }
            gonercon = 6;
            inst->alarm[8] = 40;
        }
        if (gonercon == 7) {
            Instance* hg_inst = Runner_createInstance(runner, 176.0, 16.0, 596);
            if (hg_inst && C->hg >= 0)
                Instance_setSelfVar(inst, C->hg, RValue_makeReal((GMLReal)hg_inst->instanceId));
            gonercon = 8;
        }
        if (gonercon == 10) {
            
            Instance* ws_inst = asriel_findInstanceById(runner, (int32_t)ws_id);
            if (ws_inst && C->con >= 0) Instance_setSelfVar(ws_inst, C->con, RValue_makeReal(2.0));
            if (C->shrug >= 0) Instance_setSelfVar(inst, C->shrug, RValue_makeReal(0.0));
            if (C->specialnormal >= 0) Instance_setSelfVar(inst, C->specialnormal, RValue_makeReal(1.0));
            if (C->gFaceemotion >= 0) globalSet(ctx, C->gFaceemotion, RValue_makeReal(0.0));
            if (C->gFlag >= 0) globalArraySet(ctx, C->gFlag, 20, RValue_makeReal(0.0));
            asriel_withObjectDestroy(runner, C->obj595);
            if (C->cr >= 0) {
                GMLReal snd = selfReal(inst, C->cr);
                asriel_casterPlay(ctx, snd, 0.9, 0.8);
            }
            
            if (C->objHeart >= 0) {
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t i = 0; i < n; i++) {
                    Instance* h = runner->instances[i];
                    if (h->active && h->objectIndex == C->objHeart) h->imageAlpha = 1.0f;
                }
            }
            inst->imageAlpha = 0.0f;
            
            if (C->obj744 >= 0) {
                GMLReal ib2 = 0, ib3 = 0;
                if (C->gIdealborder >= 0) {
                    int64_t k2 = ((int64_t)C->gIdealborder << 32) | 2u;
                    int64_t k3 = ((int64_t)C->gIdealborder << 32) | 3u;
                    ptrdiff_t i2 = hmgeti(ctx->globalArrayMap, k2);
                    ptrdiff_t i3 = hmgeti(ctx->globalArrayMap, k3);
                    if (i2 >= 0) ib2 = RValue_toReal(ctx->globalArrayMap[i2].value);
                    if (i3 >= 0) ib3 = RValue_toReal(ctx->globalArrayMap[i3].value);
                }
                GMLReal targetY = (ib2 + ib3) / 2.0;
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t i = 0; i < n; i++) {
                    Instance* it = runner->instances[i];
                    if (it->active && it->objectIndex == C->obj744) {
                        if (C->ignore_border >= 0) Instance_setSelfVar(it, C->ignore_border, RValue_makeReal(0.0));
                        it->x = 312.0f;
                        it->y = (float)targetY;
                    }
                }
            }
            gonercon = 11;
        }
        if (gonercon == 11) {
            if (C->objHeart >= 0) {
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t i = 0; i < n; i++) {
                    Instance* h = runner->instances[i];
                    if (h->active && h->objectIndex == C->objHeart) h->depth = 0;
                }
            }
            inst->imageAlpha += 0.1f;
            if (inst->imageAlpha >= 1.0f) {
                inst->imageAlpha = 1.0f;
                gonercon = 12;
                inst->alarm[8] = 30;
            }
        }
        if (gonercon == 13) {
            if (C->gMnfight >= 0) globalSet(ctx, C->gMnfight, RValue_makeReal(5.0));
            gonercon = 0;
        }
        if (C->gonercon >= 0) Instance_setSelfVar(inst, C->gonercon, RValue_makeReal((GMLReal)gonercon));
    }

    

    
    int32_t specialnormal = (C->specialnormal >= 0) ? selfInt(inst, C->specialnormal) : 0;
    if (specialnormal == 1) {
        GMLReal sn = (C->sn >= 0) ? selfReal(inst, C->sn) : 0.0;
        sn += 1.0;
        inst->y = 50.0f + (float)(sin(sn / 8.0) * 4.0);
        if (C->sn >= 0) Instance_setSelfVar(inst, C->sn, RValue_makeReal(sn));
    }

    GMLReal mnfight = (C->gMnfight >= 0) ? globalReal(ctx, C->gMnfight) : 0.0;
    GMLReal myfight = (C->gMyfight >= 0) ? globalReal(ctx, C->gMyfight) : 0.0;
    GMLReal bmenuno = (C->gBmenuno >= 0) ? globalReal(ctx, C->gBmenuno) : 0.0;
    if (specialnormal == 0 && mnfight == 0.0 && myfight == 0.0 &&
        (bmenuno == 1.0 || bmenuno == 2.0)) {

        
        const char* mon_name = "";
        char* monNameAlloc = NULL;
        {
            BuiltinFunc gt = VMBuiltins_find("scr_gettext");
            if (gt) {
                RValue arg = RValue_makeString("monstername_99");
                RValue result = gt(ctx, &arg, 1);
                RValue_free(&arg);
                monNameAlloc = RValue_toString(result);
                mon_name = monNameAlloc ? monNameAlloc : "";
                RValue_free(&result);
            }
        }

        
        size_t mnLen = strlen(mon_name);
        char* fullphrase = (char*)safeMalloc(mnLen + 3);
        fullphrase[0] = ' ';
        memcpy(fullphrase + 1, mon_name, mnLen);
        fullphrase[1 + mnLen] = ' ';
        fullphrase[2 + mnLen] = '\0';

        
        const char* lang = (C->gLanguage >= 0) ? globalString(ctx, C->gLanguage) : NULL;
        bool isJa = (lang && strcmp(lang, "ja") == 0);

        
        if (C->gMonstername >= 0) {
            
            
            
            int32_t padLen = (int32_t)mnLen;
            if (!isJa) padLen += 2;
            char* pad = (char*)safeMalloc((size_t)padLen * (isJa ? 3 : 1) + 1);
            int32_t idx = 0;
            for (int32_t k = 0; k < (isJa ? padLen : padLen); k++) {
                if (isJa) {
                    
                    pad[idx++] = (char)0xE3;
                    pad[idx++] = (char)0x80;
                    pad[idx++] = (char)0x80;
                } else {
                    pad[idx++] = ' ';
                }
            }
            pad[idx] = '\0';
            globalArraySet(ctx, C->gMonstername, 0, RValue_makeOwnedString(pad));
        }

        
        if (C->objInstawriter >= 0 && findInstanceByObject(runner, C->objInstawriter) != NULL) {
            BuiltinFunc gt = VMBuiltins_find("scr_gettext");
            if (gt && C->originalstring >= 0) {
                RValue arg = RValue_makeString("battle_name_header");
                RValue header = gt(ctx, &arg, 1);
                RValue_free(&arg);
                char* hdrStr = RValue_toString(header);
                RValue_free(&header);
                const char* mn0 = NULL;
                char* mn0Alloc = NULL;
                if (C->gMonstername >= 0) {
                    int64_t mk = ((int64_t)C->gMonstername << 32) | 0u;
                    ptrdiff_t mi = hmgeti(ctx->globalArrayMap, mk);
                    if (mi >= 0) {
                        mn0Alloc = RValue_toString(ctx->globalArrayMap[mi].value);
                        mn0 = mn0Alloc;
                    }
                }
                size_t hLen = hdrStr ? strlen(hdrStr) : 0;
                size_t mLen = mn0 ? strlen(mn0) : 0;
                char* combined = (char*)safeMalloc(hLen + mLen + 1);
                if (hdrStr) memcpy(combined, hdrStr, hLen);
                if (mn0) memcpy(combined + hLen, mn0, mLen);
                combined[hLen + mLen] = '\0';
                
                
                
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t i = 0; i < n; i++) {
                    Instance* it = runner->instances[i];
                    if (it->active && it->objectIndex == C->objInstawriter) {
                        Instance_setSelfVar(it, C->originalstring, RValue_makeString(combined));
                    }
                }
                free(combined);
                free(hdrStr);
                free(mn0Alloc);
            }
        }

        if (C->n_siner >= 0) {
            GMLReal ns = selfReal(inst, C->n_siner) + 1.0;
            Instance_setSelfVar(inst, C->n_siner, RValue_makeReal(ns));
        }
        nativeSetFont(r, ctx, 1);
        float textx = isJa ? 104.0f : 110.0f;

        
        int32_t fpLen = (int32_t)strlen(fullphrase);
        int32_t pos = 0;
        int32_t charIdx = 0;
        while (pos < fpLen) {
            int32_t startPos = pos;
            uint16_t ch = TextUtils_decodeUtf8(fullphrase, fpLen, &pos);
            int32_t byteLen = pos - startPos;

            uint32_t tcolor = nativeMakeColorHsvBGR((double)siner * 8.0 + (double)charIdx * 8.0, 140.0, 255.0);
            r->drawColor = tcolor;

            char letter[8] = {0};
            if (byteLen > 0 && byteLen < 8) memcpy(letter, fullphrase + startPos, (size_t)byteLen);
            char* processed = TextUtils_preprocessGmlTextIfNeeded(runner, letter);
            float tx = textx + (float)(sin(((double)siner + (double)charIdx) / 5.0) * 8.0);
            float ty = 270.0f + (float)(cos(((double)siner + (double)charIdx) / 5.0) * 4.0);
            r->vtable->drawText(r, processed, tx, ty, 1.0f, 1.0f, 0.0f);
            free(processed);

            if (isJa) {
                uint16_t code = ch;
                if (code == 32 || code >= 65377) textx += 13.0f;
                else if (code < 8192) textx += 16.0f;
                else textx += 26.0f;
            } else {
                textx += 16.0f;
            }
            charIdx++;
        }

        if (C->fullphrase >= 0)
            Instance_setSelfVar(inst, C->fullphrase, RValue_makeString(fullphrase));
        if (C->len >= 0)        Instance_setSelfVar(inst, C->len, RValue_makeReal((GMLReal)charIdx));
        free(fullphrase);
        free(monNameAlloc);
    }

    
    if (C->gFlag >= 0 && C->shrug >= 0) {
        GMLReal f20 = 0.0;
        int64_t fk = ((int64_t)C->gFlag << 32) | 20u;
        ptrdiff_t fi = hmgeti(ctx->globalArrayMap, fk);
        if (fi >= 0) f20 = RValue_toReal(ctx->globalArrayMap[fi].value);
        Instance_setSelfVar(inst, C->shrug, RValue_makeReal(f20 == 1.0 ? 1.0 : 0.0));
    }

    
    Instance_setSelfVar(inst, C->siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, C->rely,  RValue_makeReal(rely));
    if (C->normal >= 0) Instance_setSelfVar(inst, C->normal, RValue_makeReal((GMLReal)normal));

    r->drawAlpha = 1.0f;
    #undef C
}







static struct {
    int32_t spec, rno, r, rang, raspeed, rspeed;
    bool ready;
} mhdCache = { .ready = false };

static void initMhdCache(DataWin* dw) {
    mhdCache.spec    = findSelfVarId(dw, "spec");
    mhdCache.rno     = findSelfVarId(dw, "rno");
    mhdCache.r       = findSelfVarId(dw, "r");
    mhdCache.rang    = findSelfVarId(dw, "rang");
    mhdCache.raspeed = findSelfVarId(dw, "raspeed");
    mhdCache.rspeed  = findSelfVarId(dw, "rspeed");
    mhdCache.ready = (mhdCache.spec >= 0 && mhdCache.rno >= 0 && mhdCache.r >= 0 &&
                      mhdCache.rang >= 0);
}

static void native_mhd_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!mhdCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t spec = selfInt(inst, mhdCache.spec);
    int32_t rno0 = (int32_t)RValue_toReal(selfArrayGet(inst, mhdCache.rno, 0));
    GMLReal r0    = RValue_toReal(selfArrayGet(inst, mhdCache.r, 0));
    GMLReal rang0 = RValue_toReal(selfArrayGet(inst, mhdCache.rang, 0));
    GMLReal raspd0 = (mhdCache.raspeed >= 0) ? RValue_toReal(selfArrayGet(inst, mhdCache.raspeed, 0)) : 0.0;
    GMLReal rspd0  = (mhdCache.rspeed  >= 0) ? RValue_toReal(selfArrayGet(inst, mhdCache.rspeed,  0)) : 0.0;
    float alpha = inst->imageAlpha;

    if (spec == 0) {
        for (int32_t i = 0; i < rno0; i++) {
            double ang = rang0 + (double)i * 360.0 / (double)rno0;
            double rad = ang * (M_PI / 180.0);
            float xx = inst->x + (float)(r0 * cos(rad));
            float yy = inst->y + (float)(-r0 * sin(rad));
            Renderer_drawSpriteExt(r, 2515, 0, xx, yy, 1.0f, 1.0f, 0.0f, 0xFFFFFFu, alpha);
        }
    } else if (spec == 1) {
        
        
        
        static const uint32_t palette[6] = {
            16776960u, 16711680u, 32768u, 65535u, 4235519u, 8388736u
        };
        for (int32_t i = 0; i < rno0; i++) {
            double ang = rang0 + (double)i * 360.0 / (double)rno0;
            double rad = ang * (M_PI / 180.0);
            float xx = inst->x + (float)(r0 * cos(rad));
            float yy = inst->y + (float)(-r0 * sin(rad));
            uint32_t col = (i >= 0 && i < 6) ? palette[i] : 0xFFFFFFu;
            Renderer_drawSpriteExt(r, 2516, 0, xx, yy, 1.0f, 1.0f, 0.0f, col, alpha);
        }
    }

    
    rang0 += raspd0;
    r0    += rspd0;
    selfArraySet(inst, mhdCache.rang, 0, RValue_makeReal(rang0));
    selfArraySet(inst, mhdCache.r,    0, RValue_makeReal(r0));

    if (inst->imageAlpha < 1.0f) inst->imageAlpha += 0.02f;

    if (spec == 1 && r0 > 30.0) {
        r0 = 30.0;
        selfArraySet(inst, mhdCache.r, 0, RValue_makeReal(r0));
        inst->alarm[5] = -1;
        if (mhdCache.rspeed >= 0)
            selfArraySet(inst, mhdCache.rspeed, 0, RValue_makeReal(0.0));
        inst->depth = -2;
    }
}





#define STRANGETANGLE_MAX_COORD_VARIDS 8
static struct {
    int32_t active, siner, w, h;
    
    
    
    int32_t x1Candidates[STRANGETANGLE_MAX_COORD_VARIDS];
    int32_t x1CandidateCount;
    int32_t y1Candidates[STRANGETANGLE_MAX_COORD_VARIDS];
    int32_t y1CandidateCount;
    bool ready;
} strangetangleCache = { .ready = false };

static void initStrangetangleCache(DataWin* dw) {
    strangetangleCache.active = findSelfVarId(dw, "active");
    strangetangleCache.siner  = findSelfVarId(dw, "siner");
    strangetangleCache.w      = findSelfVarId(dw, "w");
    strangetangleCache.h      = findSelfVarId(dw, "h");
    strangetangleCache.x1CandidateCount = findAllSelfVarIds(dw, "x1",
                                                            strangetangleCache.x1Candidates,
                                                            STRANGETANGLE_MAX_COORD_VARIDS);
    strangetangleCache.y1CandidateCount = findAllSelfVarIds(dw, "y1",
                                                            strangetangleCache.y1Candidates,
                                                            STRANGETANGLE_MAX_COORD_VARIDS);
    strangetangleCache.ready = (strangetangleCache.active >= 0 && strangetangleCache.siner >= 0 &&
                                strangetangleCache.x1CandidateCount > 0 &&
                                strangetangleCache.y1CandidateCount > 0);
    if (strangetangleCache.x1CandidateCount > 1 || strangetangleCache.y1CandidateCount > 1) {
        fprintf(stderr, "NativeScripts: strangetangle 'x1'/'y1' have %d/%d VARI entries — per-instance resolve enabled\n",
                strangetangleCache.x1CandidateCount, strangetangleCache.y1CandidateCount);
    }
}



static int32_t resolveSelfArrayVarIdForInst(Instance* inst, const int32_t* candidates, int32_t count) {
    for (int32_t i = 0; i < count; i++) {
        int64_t key = ((int64_t)candidates[i] << 32) | 0u;  
        if (hmgeti(inst->selfArrayMap, key) >= 0) return candidates[i];
    }
    return (count > 0) ? candidates[0] : -1;
}

static void native_strangetangle_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!strangetangleCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    int32_t active = selfInt(inst, strangetangleCache.active);
    if (active == 1) {
        
        
        int32_t x1Id = resolveSelfArrayVarIdForInst(inst, strangetangleCache.x1Candidates,
                                                    strangetangleCache.x1CandidateCount);
        int32_t y1Id = resolveSelfArrayVarIdForInst(inst, strangetangleCache.y1Candidates,
                                                    strangetangleCache.y1CandidateCount);
        r->drawAlpha = inst->imageAlpha;
        r->drawColor = 0xFFFFFFu;
        
        
        
        for (int32_t i = 0; i < 30; i++) {
            GMLReal x1v = RValue_toReal(selfArrayGet(inst, x1Id, i));
            GMLReal y1v = RValue_toReal(selfArrayGet(inst, y1Id, i));
            drawFilledRect(r, (float)x1v, (float)y1v, (float)x1v + 16.0f, (float)y1v + 16.0f);
        }
        r->drawAlpha = 1.0f;
    }

    GMLReal siner = selfReal(inst, strangetangleCache.siner) + 1.0;
    GMLReal w = (strangetangleCache.w >= 0) ? selfReal(inst, strangetangleCache.w) : 0.0;
    GMLReal h = (strangetangleCache.h >= 0) ? selfReal(inst, strangetangleCache.h) : 0.0;
    double delta = sin(siner / 5.0) * 2.0;
    w += delta;
    h += delta;
    Instance_setSelfVar(inst, strangetangleCache.siner, RValue_makeReal(siner));
    if (strangetangleCache.w >= 0) Instance_setSelfVar(inst, strangetangleCache.w, RValue_makeReal(w));
    if (strangetangleCache.h >= 0) Instance_setSelfVar(inst, strangetangleCache.h, RValue_makeReal(h));
}





static struct {
    int32_t xprev, yprev;
    bool ready;
} ultimatrailCache = { .ready = false };

static void initUltimatrailCache(DataWin* dw) {
    ultimatrailCache.xprev = findSelfVarId(dw, "xprev");
    ultimatrailCache.yprev = findSelfVarId(dw, "yprev");
    ultimatrailCache.ready = (ultimatrailCache.xprev >= 0 && ultimatrailCache.yprev >= 0);
}

static void native_ultimatrail_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!ultimatrailCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    
    for (int32_t i = 12; i > 0; i--) {
        RValue xv = selfArrayGet(inst, ultimatrailCache.xprev, i - 1);
        RValue yv = selfArrayGet(inst, ultimatrailCache.yprev, i - 1);
        selfArraySet(inst, ultimatrailCache.xprev, i, xv);
        selfArraySet(inst, ultimatrailCache.yprev, i, yv);
    }
    selfArraySet(inst, ultimatrailCache.xprev, 0, RValue_makeReal((GMLReal)inst->x));
    selfArraySet(inst, ultimatrailCache.yprev, 0, RValue_makeReal((GMLReal)inst->y));

    float alpha = r->drawAlpha;
    uint32_t col = inst->imageBlend;
    
    float xp4  = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.xprev, 4));
    float yp4  = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.yprev, 4));
    float xp8  = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.xprev, 8));
    float yp8  = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.yprev, 8));
    float xp10 = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.xprev, 10));
    float yp10 = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.yprev, 10));
    float xp12 = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.xprev, 12));
    float yp12 = (float)RValue_toReal(selfArrayGet(inst, ultimatrailCache.yprev, 12));

    r->vtable->drawLine(r, xp10, yp10, xp12, yp12, 2.0f, col, alpha);
    r->vtable->drawLine(r, xp8,  yp8,  xp10, yp10, 4.0f, col, alpha);
    r->vtable->drawLine(r, xp4,  yp4,  xp8,  yp8,  6.0f, col, alpha);
    r->vtable->drawLine(r, inst->x, inst->y, xp4, yp4, 8.0f, col, alpha);
}






static void native_ultimabullet_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (runner->renderer == NULL) return;
    Renderer_drawSpriteExt(runner->renderer, inst->spriteIndex, (int32_t)inst->imageIndex,
                           inst->x, inst->y, inst->imageXscale, inst->imageYscale,
                           inst->direction, inst->imageBlend, inst->imageAlpha);
}







static struct {
    int32_t timer, siner, col_v, col2_v, col3_v;
    int32_t beamtime, hits, shaken;
    int32_t menu, menux, menuy;
    int32_t svol1, svol2, s1, s2;
    int32_t beamsfx, beamsfx2;
    int32_t ar, bw, mbw, ob_v;
    int32_t range_v, home_v, last_v;
    int32_t targetx, targety, tt, tt2, factor;
    int32_t blcon, blconwd;
    int32_t gMnfight, gMsc, gMsg, gTyper, gMercy, gBmenucoord;
    int32_t objBtparent, objHeart, objFightbt, objItembt, objSparebt;
    int32_t scrBinfowrite;
    int32_t scrGettext;  
    bool ready;
} lastbeamCache = { .ready = false };

static void initLastbeamCache(VMContext* ctx, DataWin* dw) {
    lastbeamCache.timer     = findSelfVarId(dw, "timer");
    lastbeamCache.siner     = findSelfVarId(dw, "siner");
    lastbeamCache.col_v     = findSelfVarId(dw, "col");
    lastbeamCache.col2_v    = findSelfVarId(dw, "col2");
    lastbeamCache.col3_v    = findSelfVarId(dw, "col3");
    lastbeamCache.beamtime  = findSelfVarId(dw, "beamtime");
    lastbeamCache.hits      = findSelfVarId(dw, "hits");
    lastbeamCache.shaken    = findSelfVarId(dw, "shaken");
    lastbeamCache.menu      = findSelfVarId(dw, "menu");
    lastbeamCache.menux     = findSelfVarId(dw, "menux");
    lastbeamCache.menuy     = findSelfVarId(dw, "menuy");
    lastbeamCache.svol1     = findSelfVarId(dw, "svol1");
    lastbeamCache.svol2     = findSelfVarId(dw, "svol2");
    lastbeamCache.s1        = findSelfVarId(dw, "s1");
    lastbeamCache.s2        = findSelfVarId(dw, "s2");
    lastbeamCache.beamsfx   = findSelfVarId(dw, "beamsfx");
    lastbeamCache.beamsfx2  = findSelfVarId(dw, "beamsfx2");
    lastbeamCache.ar        = findSelfVarId(dw, "ar");
    lastbeamCache.bw        = findSelfVarId(dw, "bw");
    lastbeamCache.mbw       = findSelfVarId(dw, "mbw");
    lastbeamCache.ob_v      = findSelfVarId(dw, "ob");
    lastbeamCache.range_v   = findSelfVarId(dw, "range");
    lastbeamCache.home_v    = findSelfVarId(dw, "home");
    lastbeamCache.last_v    = findSelfVarId(dw, "last");
    lastbeamCache.targetx   = findSelfVarId(dw, "targetx");
    lastbeamCache.targety   = findSelfVarId(dw, "targety");
    lastbeamCache.tt        = findSelfVarId(dw, "tt");
    lastbeamCache.tt2       = findSelfVarId(dw, "tt2");
    lastbeamCache.factor    = findSelfVarId(dw, "factor");
    lastbeamCache.blcon     = findSelfVarId(dw, "blcon");
    lastbeamCache.blconwd   = findSelfVarId(dw, "blconwd");
    lastbeamCache.gMnfight     = findGlobalVarId(ctx, "mnfight");
    lastbeamCache.gMsc         = findGlobalVarId(ctx, "msc");
    lastbeamCache.gMsg         = findGlobalVarId(ctx, "msg");
    lastbeamCache.gTyper       = findGlobalVarId(ctx, "typer");
    lastbeamCache.gMercy       = findGlobalVarId(ctx, "mercy");
    lastbeamCache.gBmenucoord  = findGlobalVarId(ctx, "bmenucoord");
    lastbeamCache.objBtparent  = findObjectIndex(dw, "obj_btparent");
    lastbeamCache.objHeart     = findObjectIndex(dw, "obj_heart");
    lastbeamCache.objFightbt   = findObjectIndex(dw, "obj_fightbt");
    lastbeamCache.objItembt    = findObjectIndex(dw, "obj_itembt");
    lastbeamCache.objSparebt   = findObjectIndex(dw, "obj_sparebt");
    lastbeamCache.scrBinfowrite = findScriptCodeId(ctx, "scr_binfowrite");
    lastbeamCache.ready = (lastbeamCache.timer >= 0 && lastbeamCache.siner >= 0 &&
                           lastbeamCache.beamtime >= 0 && lastbeamCache.bw >= 0);
}



static GMLReal lastbeam_casterPlay(VMContext* ctx, GMLReal snd, GMLReal gain, GMLReal pitch) {
    GMLReal inst_id = 0.0;
    BuiltinFunc aps = VMBuiltins_find("audio_play_sound");
    if (aps) {
        RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(100.0), RValue_makeReal(0.0) };
        RValue r = aps(ctx, a, 3);
        inst_id = RValue_toReal(r);
        RValue_free(&r);
    }
    BuiltinFunc asp = VMBuiltins_find("audio_sound_pitch");
    if (asp) { RValue a[2] = { RValue_makeReal(snd), RValue_makeReal(pitch) }; RValue r = asp(ctx, a, 2); RValue_free(&r); }
    BuiltinFunc asg = VMBuiltins_find("audio_sound_gain");
    if (asg) { RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(gain), RValue_makeReal(0.0) }; RValue r = asg(ctx, a, 3); RValue_free(&r); }
    return inst_id;
}
static GMLReal lastbeam_casterLoop(VMContext* ctx, GMLReal snd, GMLReal gain, GMLReal pitch) {
    GMLReal inst_id = 0.0;
    BuiltinFunc aps = VMBuiltins_find("audio_play_sound");
    if (aps) {
        RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(120.0), RValue_makeReal(1.0) };
        RValue r = aps(ctx, a, 3);
        inst_id = RValue_toReal(r);
        RValue_free(&r);
    }
    BuiltinFunc asp = VMBuiltins_find("audio_sound_pitch");
    if (asp) { RValue a[2] = { RValue_makeReal(snd), RValue_makeReal(pitch) }; RValue r = asp(ctx, a, 2); RValue_free(&r); }
    BuiltinFunc asg = VMBuiltins_find("audio_sound_gain");
    if (asg) { RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(gain), RValue_makeReal(0.0) }; RValue r = asg(ctx, a, 3); RValue_free(&r); }
    return inst_id;
}
static void lastbeam_casterSetVolume(VMContext* ctx, GMLReal snd, GMLReal gain) {
    BuiltinFunc asg = VMBuiltins_find("audio_sound_gain");
    if (asg) { RValue a[3] = { RValue_makeReal(snd), RValue_makeReal(gain), RValue_makeReal(0.0) }; RValue r = asg(ctx, a, 3); RValue_free(&r); }
}
static void lastbeam_casterSetPitch(VMContext* ctx, GMLReal snd, GMLReal pitch) {
    BuiltinFunc asp = VMBuiltins_find("audio_sound_pitch");
    if (asp) { RValue a[2] = { RValue_makeReal(snd), RValue_makeReal(pitch) }; RValue r = asp(ctx, a, 2); RValue_free(&r); }
}
static void lastbeam_casterStop(VMContext* ctx, GMLReal snd) {
    BuiltinFunc ass = VMBuiltins_find("audio_stop_sound");
    if (ass) { RValue a[1] = { RValue_makeReal(snd) }; RValue r = ass(ctx, a, 1); RValue_free(&r); }
}

static void native_lastbeam_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!lastbeamCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    GMLReal timer = selfReal(inst, lastbeamCache.timer) + 1.0;
    GMLReal siner = selfReal(inst, lastbeamCache.siner) + 1.0;
    uint32_t col  = nativeMakeColorHsvBGR(siner * 11.0,           190.0, 250.0);
    uint32_t col2 = nativeMakeColorHsvBGR((siner + 3.0) * 11.0,   190.0, 250.0);
    uint32_t col3 = nativeMakeColorHsvBGR((siner + 5.0) * 11.0,   140.0, 250.0);
    if (lastbeamCache.col_v  >= 0) Instance_setSelfVar(inst, lastbeamCache.col_v,  RValue_makeReal((GMLReal)col));
    if (lastbeamCache.col2_v >= 0) Instance_setSelfVar(inst, lastbeamCache.col2_v, RValue_makeReal((GMLReal)col2));
    if (lastbeamCache.col3_v >= 0) Instance_setSelfVar(inst, lastbeamCache.col3_v, RValue_makeReal((GMLReal)col3));

    GMLReal beamtime = selfReal(inst, lastbeamCache.beamtime);

    
    if ((int32_t)timer == (int32_t)beamtime) {
        if (lastbeamCache.hits >= 0) Instance_setSelfVar(inst, lastbeamCache.hits, RValue_makeReal(0.0));
        inst->alarm[5] = 1;
        
        if (lastbeamCache.objBtparent >= 0) {
            int32_t n = (int32_t)arrlen(runner->instances);
            for (int32_t k = 0; k < n; k++) {
                Instance* it = runner->instances[k];
                if (it->active && it->objectIndex == lastbeamCache.objBtparent) it->depth = -2000;
            }
        }
        inst->depth = -1000;
        if (lastbeamCache.objHeart >= 0) {
            int32_t n = (int32_t)arrlen(runner->instances);
            for (int32_t k = 0; k < n; k++) {
                Instance* it = runner->instances[k];
                if (it->active && it->objectIndex == lastbeamCache.objHeart) it->depth = -2100;
            }
        }
        if (lastbeamCache.shaken >= 0) Instance_setSelfVar(inst, lastbeamCache.shaken, RValue_makeReal(0.0));
        
        int32_t btIdx[3] = { lastbeamCache.objFightbt, lastbeamCache.objItembt, lastbeamCache.objSparebt };
        GMLReal btObjId[3] = { 751.0, 753.0, 755.0 };
        for (int32_t j = 0; j < 3; j++) {
            if (lastbeamCache.menu >= 0)  selfArraySet(inst, lastbeamCache.menu,  j, RValue_makeReal(btObjId[j]));
            float btx = 0.0f, bty = 0.0f;
            if (btIdx[j] >= 0) {
                Instance* bt = findInstanceByObject(runner, btIdx[j]);
                if (bt) { btx = bt->x; bty = bt->y; }
            }
            if (lastbeamCache.menux >= 0) selfArraySet(inst, lastbeamCache.menux, j, RValue_makeReal((GMLReal)btx));
            if (lastbeamCache.menuy >= 0) selfArraySet(inst, lastbeamCache.menuy, j, RValue_makeReal((GMLReal)bty));
        }
        if (lastbeamCache.svol1 >= 0) Instance_setSelfVar(inst, lastbeamCache.svol1, RValue_makeReal(0.8));
        if (lastbeamCache.svol2 >= 0) Instance_setSelfVar(inst, lastbeamCache.svol2, RValue_makeReal(0.0));
        
        GMLReal beamsfx  = (lastbeamCache.beamsfx  >= 0) ? selfReal(inst, lastbeamCache.beamsfx)  : 0.0;
        GMLReal beamsfx2 = (lastbeamCache.beamsfx2 >= 0) ? selfReal(inst, lastbeamCache.beamsfx2) : 0.0;
        GMLReal s1id = lastbeam_casterPlay(ctx, beamsfx,  0.8, 1.0);
        GMLReal s2id = lastbeam_casterLoop(ctx, beamsfx2, 1.0, 1.0);
        if (lastbeamCache.s1 >= 0) Instance_setSelfVar(inst, lastbeamCache.s1, RValue_makeReal(s1id));
        if (lastbeamCache.s2 >= 0) Instance_setSelfVar(inst, lastbeamCache.s2, RValue_makeReal(s2id));
        if (lastbeamCache.ar >= 0) Instance_setSelfVar(inst, lastbeamCache.ar, RValue_makeReal(0.7));
        
        int32_t range_v = (lastbeamCache.range_v >= 0) ? selfInt(inst, lastbeamCache.range_v) : 0;
        GMLReal bw_init = 60.0;
        if (range_v == 1) bw_init = 220.0;
        if (range_v == 2) bw_init = 120.0;
        if (lastbeamCache.mbw >= 0) Instance_setSelfVar(inst, lastbeamCache.mbw, RValue_makeReal(bw_init));
        if (lastbeamCache.bw  >= 0) Instance_setSelfVar(inst, lastbeamCache.bw,  RValue_makeReal(0.0));
    }

    
    if (timer > beamtime) {
        GMLReal mbw = selfReal(inst, lastbeamCache.mbw);
        GMLReal bw  = selfReal(inst, lastbeamCache.bw);
        if (timer < beamtime + 6.0) bw += mbw / 5.0;

        if (bw > 0.0) {
            GMLReal svol2 = (lastbeamCache.svol2 >= 0) ? selfReal(inst, lastbeamCache.svol2) : 0.0;
            if (svol2 < 0.8) svol2 += 0.05;
            GMLReal s2id = (lastbeamCache.s2 >= 0) ? selfReal(inst, lastbeamCache.s2) : 0.0;
            lastbeam_casterSetVolume(ctx, s2id, svol2);
            if (lastbeamCache.svol2 >= 0) Instance_setSelfVar(inst, lastbeamCache.svol2, RValue_makeReal(svol2));

            GMLReal ob = sin(siner / 2.0) * (mbw / 5.0) * (bw / mbw);
            if (lastbeamCache.ob_v >= 0) Instance_setSelfVar(inst, lastbeamCache.ob_v, RValue_makeReal(ob));

            GMLReal ar = (lastbeamCache.ar >= 0) ? selfReal(inst, lastbeamCache.ar) : 0.7;
            r->drawAlpha = (float)ar;
            int32_t home_v = (lastbeamCache.home_v >= 0) ? selfInt(inst, lastbeamCache.home_v) : 0;
            float rh = (runner->currentRoom ? (float)runner->currentRoom->height : 480.0f);

            if (home_v == 0) {
                
                r->vtable->drawTriangleColor(r, inst->x, inst->y,
                                             inst->x + (float)(bw + ob), rh + 10.0f,
                                             inst->x - (float)(bw + ob), rh + 10.0f,
                                             col, col2, col2, (float)ar, false);
                r->vtable->drawTriangleColor(r, inst->x, inst->y,
                                             inst->x + (float)(bw + ob * 0.5), rh + 10.0f,
                                             inst->x - (float)(bw + ob * 0.5), rh + 10.0f,
                                             col, col2, col2, (float)ar, false);
                r->vtable->drawTriangleColor(r, inst->x, inst->y,
                                             inst->x + (float)(bw - ob), rh + 10.0f,
                                             inst->x - (float)(bw - ob), rh + 10.0f,
                                             col, col3, col3, (float)ar, false);
                float sx1 = (float)((7.0 + sin(siner / 2.0) * 3.75) * (bw / mbw));
                float sx2 = (float)((6.0 + sin(siner / 2.0) * 2.5)  * (bw / mbw));
                float sx3 = (float)((5.0 + sin(siner / 2.0))        * (bw / mbw));
                Renderer_drawSpriteExt(r, 2502, 0, inst->x, inst->y, sx1, sx1, 0.0f, col,  (float)ar);
                Renderer_drawSpriteExt(r, 2502, 0, inst->x, inst->y, sx2, sx2, 0.0f, col,  (float)ar);
                Renderer_drawSpriteExt(r, 2502, 0, inst->x, inst->y, sx3, sx3, 0.0f, col2, (float)ar);
            }
            if (home_v == 1) {
                
                
                GMLReal targetx = 0.0;
                GMLReal targety = 0.0;
                if (lastbeamCache.targetx >= 0) Instance_setSelfVar(inst, lastbeamCache.targetx, RValue_makeReal(targetx));
                if (lastbeamCache.targety >= 0) Instance_setSelfVar(inst, lastbeamCache.targety, RValue_makeReal(targety));
                GMLReal dx = targetx - inst->x;
                GMLReal dy = targety - inst->y;
                GMLReal dir = atan2(-dy, dx) * (180.0 / M_PI);
                GMLReal tt  = 600.0 * cos(dir * (M_PI / 180.0));
                GMLReal tt2 = -600.0 * sin(dir * (M_PI / 180.0));
                if (lastbeamCache.tt  >= 0) Instance_setSelfVar(inst, lastbeamCache.tt,  RValue_makeReal(tt));
                if (lastbeamCache.tt2 >= 0) Instance_setSelfVar(inst, lastbeamCache.tt2, RValue_makeReal(tt2));
                r->vtable->drawLineColor(r, inst->x, inst->y, inst->x + (float)tt, inst->y + (float)tt2,
                                         (float)(bw + ob * 2.0), col, col2, (float)ar);
                r->vtable->drawLineColor(r, inst->x, inst->y, inst->x + (float)tt, inst->y + (float)tt2,
                                         (float)(bw + ob), col, col2, (float)ar);
                r->vtable->drawLineColor(r, inst->x, inst->y, inst->x + (float)tt, inst->y + (float)tt2,
                                         (float)bw, col, col3, (float)ar);
                double factor = mbw / 60.0;
                if (lastbeamCache.factor >= 0) Instance_setSelfVar(inst, lastbeamCache.factor, RValue_makeReal(factor));
                float sx1 = (float)((7.0 + sin(siner / 2.0) * 3.0) * (bw / mbw) * factor);
                float sx2 = (float)((6.0 + sin(siner / 2.0) * 2.0) * (bw / mbw) * factor);
                float sx3 = (float)((5.0 + sin(siner / 2.0))        * (bw / mbw) * factor);
                Renderer_drawSpriteExt(r, 2502, 0, inst->x, inst->y, sx1, sx1, 0.0f, col,  (float)ar);
                Renderer_drawSpriteExt(r, 2502, 0, inst->x, inst->y, sx2, sx2, 0.0f, col,  (float)ar);
                Renderer_drawSpriteExt(r, 2502, 0, inst->x, inst->y, sx3, sx3, 0.0f, col2, (float)ar);
            }

            int32_t last_v = (lastbeamCache.last_v >= 0) ? selfInt(inst, lastbeamCache.last_v) : 0;

            
            if (last_v > 0 && (int32_t)timer == 120) {
                if (lastbeamCache.shaken >= 0) Instance_setSelfVar(inst, lastbeamCache.shaken, RValue_makeReal(1.0));
                bw += 100.0;
                mbw += 80.0;
                GMLReal s2id = (lastbeamCache.s2 >= 0) ? selfReal(inst, lastbeamCache.s2) : 0.0;
                lastbeam_casterSetPitch(ctx, s2id, 1.3);
                Instance* blcon = Runner_createInstance(runner, 400.0, 50.0, 188);
                if (blcon) {
                    blcon->depth = -2000;
                    if (lastbeamCache.blcon >= 0)
                        Instance_setSelfVar(inst, lastbeamCache.blcon, RValue_makeReal((GMLReal)blcon->instanceId));
                }
                if (lastbeamCache.gMsc   >= 0) globalSet(ctx, lastbeamCache.gMsc,   RValue_makeReal(0.0));
                if (lastbeamCache.gTyper >= 0) globalSet(ctx, lastbeamCache.gTyper, RValue_makeReal(88.0));
                BuiltinFunc gt = VMBuiltins_find("scr_gettext");
                if (gt && lastbeamCache.gMsg >= 0) {
                    RValue arg = RValue_makeString("obj_lastbeam_230");
                    RValue res = gt(ctx, &arg, 1);
                    RValue_free(&arg);
                    globalArraySet(ctx, lastbeamCache.gMsg, 0, res);
                }
                if (blcon) {
                    Instance* blconwd = Runner_createInstance(runner, blcon->x + 25.0f, blcon->y + 10.0f, 786);
                    if (blconwd) {
                        blconwd->depth = -2200;
                        if (lastbeamCache.blconwd >= 0)
                            Instance_setSelfVar(inst, lastbeamCache.blconwd, RValue_makeReal((GMLReal)blconwd->instanceId));
                    }
                }
            }
            
            if ((int32_t)timer == 190 || (int32_t)timer == 340) {
                if (last_v > 0) {
                    GMLReal blconId = (lastbeamCache.blcon >= 0)   ? selfReal(inst, lastbeamCache.blcon)   : 0.0;
                    GMLReal bwId    = (lastbeamCache.blconwd >= 0) ? selfReal(inst, lastbeamCache.blconwd) : 0.0;
                    int32_t n = (int32_t)arrlen(runner->instances);
                    for (int32_t k = 0; k < n; k++) {
                        Instance* it = runner->instances[k];
                        if (!it->active) continue;
                        if ((int32_t)it->instanceId == (int32_t)blconId ||
                            (int32_t)it->instanceId == (int32_t)bwId) {
                            Runner_destroyInstance(runner, it);
                        }
                    }
                }
            }
            
            if (last_v > 0 && (int32_t)timer == 240) {
                if (lastbeamCache.shaken >= 0) Instance_setSelfVar(inst, lastbeamCache.shaken, RValue_makeReal(2.0));
                bw += 400.0;
                mbw += 260.0;
                GMLReal s2id = (lastbeamCache.s2 >= 0) ? selfReal(inst, lastbeamCache.s2) : 0.0;
                lastbeam_casterSetPitch(ctx, s2id, 1.8);
                Instance* blcon = Runner_createInstance(runner, 400.0, 50.0, 188);
                if (blcon) {
                    blcon->depth = -2000;
                    if (lastbeamCache.blcon >= 0)
                        Instance_setSelfVar(inst, lastbeamCache.blcon, RValue_makeReal((GMLReal)blcon->instanceId));
                }
                if (lastbeamCache.gMsc   >= 0) globalSet(ctx, lastbeamCache.gMsc,   RValue_makeReal(0.0));
                if (lastbeamCache.gTyper >= 0) globalSet(ctx, lastbeamCache.gTyper, RValue_makeReal(88.0));
                BuiltinFunc gt = VMBuiltins_find("scr_gettext");
                if (gt && lastbeamCache.gMsg >= 0) {
                    RValue arg = RValue_makeString("obj_lastbeam_255");
                    RValue res = gt(ctx, &arg, 1);
                    RValue_free(&arg);
                    globalArraySet(ctx, lastbeamCache.gMsg, 0, res);
                }
                if (blcon) {
                    Instance* blconwd = Runner_createInstance(runner, blcon->x + 25.0f, blcon->y + 10.0f, 786);
                    if (blconwd) {
                        blconwd->depth = -2200;
                        if (lastbeamCache.blconwd >= 0)
                            Instance_setSelfVar(inst, lastbeamCache.blconwd, RValue_makeReal((GMLReal)blconwd->instanceId));
                    }
                }
            }

            int32_t shakenVal = (lastbeamCache.shaken >= 0) ? selfInt(inst, lastbeamCache.shaken) : 0;
            if (shakenVal == 1) {
                
                BuiltinFunc rnd = VMBuiltins_find("random");
                for (int32_t j = 0; j < 3; j++) {
                    GMLReal mx = (lastbeamCache.menux >= 0) ? RValue_toReal(selfArrayGet(inst, lastbeamCache.menux, j)) : 0.0;
                    GMLReal my = (lastbeamCache.menuy >= 0) ? RValue_toReal(selfArrayGet(inst, lastbeamCache.menuy, j)) : 0.0;
                    double r1 = 0.0, r2 = 0.0;
                    if (rnd) {
                        RValue a = RValue_makeReal(4.0);
                        RValue v1 = rnd(ctx, &a, 1); r1 = RValue_toReal(v1); RValue_free(&v1);
                        RValue v2 = rnd(ctx, &a, 1); r2 = RValue_toReal(v2); RValue_free(&v2);
                    }
                    GMLReal btObjId = (lastbeamCache.menu >= 0) ? RValue_toReal(selfArrayGet(inst, lastbeamCache.menu, j)) : 0.0;
                    
                    int32_t n = (int32_t)arrlen(runner->instances);
                    for (int32_t k = 0; k < n; k++) {
                        Instance* it = runner->instances[k];
                        if (it->active && it->objectIndex == (int32_t)btObjId) {
                            it->x = (float)(mx + r1 - r2);
                            it->y = (float)(my + r1 - r2);
                            break;
                        }
                    }
                }
            }
            if (shakenVal == 2) {
                BuiltinFunc rnd = VMBuiltins_find("random");
                for (int32_t j = 0; j < 3; j++) {
                    double r1 = 0.0, r2 = 0.0;
                    if (rnd) {
                        RValue a = RValue_makeReal(4.0);
                        RValue v1 = rnd(ctx, &a, 1); r1 = RValue_toReal(v1); RValue_free(&v1);
                        RValue v2 = rnd(ctx, &a, 1); r2 = RValue_toReal(v2); RValue_free(&v2);
                    }
                    GMLReal btObjId = (lastbeamCache.menu >= 0) ? RValue_toReal(selfArrayGet(inst, lastbeamCache.menu, j)) : 0.0;
                    int32_t n = (int32_t)arrlen(runner->instances);
                    for (int32_t k = 0; k < n; k++) {
                        Instance* it = runner->instances[k];
                        if (it->active && it->objectIndex == (int32_t)btObjId) {
                            it->vspeed += 0.5f;
                            it->imageAngle += (float)(r1 - r2);
                            break;
                        }
                    }
                }
                if (lastbeamCache.gBmenucoord >= 0) globalArraySet(ctx, lastbeamCache.gBmenucoord, 0, RValue_makeReal(1.0));
                if (lastbeamCache.gMercy     >= 0) globalSet(ctx, lastbeamCache.gMercy, RValue_makeReal(3.0));
            }

            r->drawAlpha = 1.0f;

            
            if (timer > beamtime + 80.0 + (GMLReal)last_v) {
                if (svol2 > 0.0) svol2 -= 0.1;
                GMLReal s2id = (lastbeamCache.s2 >= 0) ? selfReal(inst, lastbeamCache.s2) : 0.0;
                lastbeam_casterSetVolume(ctx, s2id, svol2);
                if (lastbeamCache.svol2 >= 0) Instance_setSelfVar(inst, lastbeamCache.svol2, RValue_makeReal(svol2));
                bw -= mbw / 12.0;
                ar -= 0.04;
                if (lastbeamCache.ar >= 0) Instance_setSelfVar(inst, lastbeamCache.ar, RValue_makeReal(ar));
                if (bw <= 0.0) {
                    lastbeam_casterStop(ctx, s2id);
                    if (lastbeamCache.gMnfight >= 0) globalSet(ctx, lastbeamCache.gMnfight, RValue_makeReal(3.0));
                    BuiltinFunc gt = VMBuiltins_find("scr_gettext");
                    if (gt && lastbeamCache.gMsg >= 0) {
                        RValue arg = RValue_makeString("obj_lastbeam_296");
                        RValue res = gt(ctx, &arg, 1);
                        RValue_free(&arg);
                        globalArraySet(ctx, lastbeamCache.gMsg, 0, res);
                    }
                    Runner_destroyInstance(runner, inst);
                    return;
                }
            }
        }
        if (lastbeamCache.bw  >= 0) Instance_setSelfVar(inst, lastbeamCache.bw,  RValue_makeReal(bw));
        if (lastbeamCache.mbw >= 0) Instance_setSelfVar(inst, lastbeamCache.mbw, RValue_makeReal(mbw));
    }

    Instance_setSelfVar(inst, lastbeamCache.timer, RValue_makeReal(timer));
    Instance_setSelfVar(inst, lastbeamCache.siner, RValue_makeReal(siner));

    
    if (lastbeamCache.scrBinfowrite >= 0) {
        RValue res = VM_callCodeIndex(ctx, lastbeamCache.scrBinfowrite, NULL, 0);
        RValue_free(&res);
    }
}







static struct {
    int32_t anim, siner, side, yoff, yoff2, thiscolor;
    int32_t ar_shake, bodyfader, cry;
    int32_t armrot, rx, ry;
    int32_t ucon, psfx, arf, u_gen, gen, target;
    int32_t bcon, ps, r_break, r_al, radi, r_siner, radi_s, armx, army, beam;
    int32_t darker, darker_x;
    int32_t gFaceemotion, gMnfight, gDebug;
    int32_t objHeart;
    bool ready;
} afinalBodyCache = { .ready = false };

static void initAfinalBodyCache(VMContext* ctx, DataWin* dw) {
    afinalBodyCache.anim       = findSelfVarId(dw, "anim");
    afinalBodyCache.siner      = findSelfVarId(dw, "siner");
    afinalBodyCache.side       = findSelfVarId(dw, "side");
    afinalBodyCache.yoff       = findSelfVarId(dw, "yoff");
    afinalBodyCache.yoff2      = findSelfVarId(dw, "yoff2");
    afinalBodyCache.thiscolor  = findSelfVarId(dw, "thiscolor");
    afinalBodyCache.ar_shake   = findSelfVarId(dw, "ar_shake");
    afinalBodyCache.bodyfader  = findSelfVarId(dw, "bodyfader");
    afinalBodyCache.cry        = findSelfVarId(dw, "cry");
    afinalBodyCache.armrot     = findSelfVarId(dw, "armrot");
    afinalBodyCache.rx         = findSelfVarId(dw, "rx");
    afinalBodyCache.ry         = findSelfVarId(dw, "ry");
    afinalBodyCache.ucon       = findSelfVarId(dw, "ucon");
    afinalBodyCache.psfx       = findSelfVarId(dw, "psfx");
    afinalBodyCache.arf        = findSelfVarId(dw, "arf");
    afinalBodyCache.u_gen      = findSelfVarId(dw, "u_gen");
    afinalBodyCache.gen        = findSelfVarId(dw, "gen");
    afinalBodyCache.target     = findSelfVarId(dw, "target");
    afinalBodyCache.bcon       = findSelfVarId(dw, "bcon");
    afinalBodyCache.ps         = findSelfVarId(dw, "ps");
    afinalBodyCache.r_break    = findSelfVarId(dw, "r_break");
    afinalBodyCache.r_al       = findSelfVarId(dw, "r_al");
    afinalBodyCache.radi       = findSelfVarId(dw, "radi");
    afinalBodyCache.r_siner    = findSelfVarId(dw, "r_siner");
    afinalBodyCache.radi_s     = findSelfVarId(dw, "radi_s");
    afinalBodyCache.armx       = findSelfVarId(dw, "armx");
    afinalBodyCache.army       = findSelfVarId(dw, "army");
    afinalBodyCache.beam       = findSelfVarId(dw, "beam");
    afinalBodyCache.darker     = findSelfVarId(dw, "darker");
    afinalBodyCache.darker_x   = findSelfVarId(dw, "darker_x");
    afinalBodyCache.gFaceemotion = findGlobalVarId(ctx, "faceemotion");
    afinalBodyCache.gMnfight     = findGlobalVarId(ctx, "mnfight");
    afinalBodyCache.gDebug       = findGlobalVarId(ctx, "debug");
    afinalBodyCache.objHeart     = findObjectIndex(dw, "obj_heart");
    afinalBodyCache.ready = (afinalBodyCache.anim >= 0 && afinalBodyCache.siner >= 0 &&
                             afinalBodyCache.side >= 0 && afinalBodyCache.armrot >= 0 &&
                             afinalBodyCache.cry >= 0);
}

static void native_afinalBody_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!afinalBodyCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    #define AF (&afinalBodyCache)

    GMLReal anim  = selfReal(inst, AF->anim)  + 1.0;
    GMLReal siner = selfReal(inst, AF->siner) + 1.0;
    GMLReal side  = selfReal(inst, AF->side)  + 2.0;
    if (side > 800.0) side -= 800.0;

    GMLReal yoff  = sin(siner /  4.0);
    GMLReal yoff2 = sin(siner / 16.0);
    if (AF->yoff  >= 0) Instance_setSelfVar(inst, AF->yoff,  RValue_makeReal(yoff));
    if (AF->yoff2 >= 0) Instance_setSelfVar(inst, AF->yoff2, RValue_makeReal(yoff2));

    
    r->drawColor = 0x000000u;
    r->drawAlpha = 1.0f;
    drawFilledRect(r, -10.0f, 240.0f, 999.0f, -10.0f);

    
    uint32_t thiscolor = nativeMakeColorHsvBGR(siner * 6.0, 200.0, 200.0);
    if (AF->thiscolor >= 0) Instance_setSelfVar(inst, AF->thiscolor, RValue_makeReal((GMLReal)thiscolor));
    r->drawColor = thiscolor;
    Renderer_drawSpritePartExt(r, 2470, 0, (int32_t)side,         0, 276, 216, 640.0f, 0.0f, -1.0f, 1.0f, thiscolor, 0.5f);
    Renderer_drawSpritePartExt(r, 2470, 0, (int32_t)(side + 60.0), 0, 276, 216, 640.0f, 0.0f, -1.0f, 1.0f, thiscolor, 0.5f);
    Renderer_drawSpritePartExt(r, 2470, 0, (int32_t)(side + 120.0),0, 276, 216, 640.0f, 0.0f, -1.0f, 1.0f, thiscolor, 0.5f);
    Renderer_drawSpritePartExt(r, 2470, 0, (int32_t)side,         0, 276, 216, 0.0f,   0.0f,  1.0f, 1.0f, thiscolor, 0.5f);
    Renderer_drawSpritePartExt(r, 2470, 0, (int32_t)(side + 60.0), 0, 276, 216, 0.0f,   0.0f,  1.0f, 1.0f, thiscolor, 0.5f);
    Renderer_drawSpritePartExt(r, 2470, 0, (int32_t)(side + 120.0),0, 276, 216, 0.0f,   0.0f,  1.0f, 1.0f, thiscolor, 0.5f);

    
    uint32_t blend = inst->imageBlend;
    float alpha = inst->imageAlpha;
    int32_t anim6 = (int32_t)floor(anim / 6.0);
    Renderer_drawSpriteExt(r, 2449, anim6, inst->x + 42.0f, (inst->y - 52.0f) + (float)(yoff2 * 4.0),  2.0f, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2449, anim6, inst->x - 44.0f, (inst->y - 52.0f) + (float)(yoff2 * 4.0), -2.0f, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2444, anim6, inst->x - 110.0f, inst->y - 52.0f,  2.0f, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2444, anim6, inst->x + 108.0f, inst->y - 52.0f, -2.0f, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2448, anim6, inst->x - 2.0f,   inst->y + 146.0f, 2.0f, 2.0f, 0.0f, blend, alpha);
    Renderer_drawSpriteExt(r, 2446, anim6, inst->x - 2.0f,   inst->y + 68.0f,  2.0f, 2.0f, 0.0f, blend, alpha);

    
    GMLReal ar_shake = (AF->ar_shake >= 0) ? selfReal(inst, AF->ar_shake) : 0.0;
    double rxv = 0.0, ryv = 0.0;
    BuiltinFunc rnd = VMBuiltins_find("random");
    if (rnd) {
        RValue a = RValue_makeReal(ar_shake);
        RValue v1 = rnd(ctx, &a, 1); double a1 = RValue_toReal(v1); RValue_free(&v1);
        RValue v2 = rnd(ctx, &a, 1); double a2 = RValue_toReal(v2); RValue_free(&v2);
        RValue v3 = rnd(ctx, &a, 1); double b1 = RValue_toReal(v3); RValue_free(&v3);
        RValue v4 = rnd(ctx, &a, 1); double b2 = RValue_toReal(v4); RValue_free(&v4);
        rxv = (a1 - a2) * 0.7;
        ryv = (b1 - b2) * 1.5;
    }
    if (AF->rx >= 0) Instance_setSelfVar(inst, AF->rx, RValue_makeReal(rxv));
    if (AF->ry >= 0) Instance_setSelfVar(inst, AF->ry, RValue_makeReal(ryv));

    
    GMLReal bodyfader = (AF->bodyfader >= 0) ? selfReal(inst, AF->bodyfader) : 0.0;
    r->drawAlpha = (float)bodyfader;
    r->drawColor = 0x000000u;
    drawFilledRect(r, -10.0f, -10.0f, 999.0f, 999.0f);
    r->drawAlpha = 1.0f;

    
    int32_t cry = selfInt(inst, AF->cry);
    if (cry == 0) {
        int32_t face = (AF->gFaceemotion >= 0) ? (int32_t)globalReal(ctx, AF->gFaceemotion) : 0;
        Renderer_drawSpriteExt(r, 2450, face, inst->x, inst->y, 2.0f, 2.0f, 0.0f, blend, alpha);
    }
    if (cry == 1) {
        Renderer_drawSpriteExt(r, 2451, (int32_t)floor(siner / 8.0),
                               inst->x + (float)(rxv / 3.0), inst->y + (float)(ryv / 3.0),
                               2.0f, 2.0f, 0.0f, blend, alpha);
    }
    if (cry == 2) {
        Renderer_drawSpriteExt(r, 2452, (int32_t)floor(siner / 2.0),
                               inst->x + (float)(rxv / 3.0), inst->y + (float)(ryv / 3.0),
                               2.0f, 2.0f, 0.0f, blend, alpha);
    }

    
    GMLReal armrot = selfReal(inst, AF->armrot);
    float armAlphaBody = alpha - (float)bodyfader;
    Renderer_drawSpriteExt(r, 2454, anim6, (inst->x - 58.0f) + (float)rxv,
                           inst->y + 56.0f + (float)(yoff * 2.0) + (float)ryv,
                           2.0f, 2.0f, (float)armrot, blend, armAlphaBody);
    Renderer_drawSpriteExt(r, 2454, anim6, inst->x + 56.0f + (float)rxv,
                           inst->y + 56.0f + (float)(yoff * 2.0) + (float)ryv,
                           -2.0f, 2.0f, (float)(-armrot), blend, armAlphaBody);
    Renderer_drawSpriteExt(r, 2455, anim6, inst->x - 84.0f, inst->y + 32.0f,  2.0f, 2.0f, 0.0f, blend, armAlphaBody);
    Renderer_drawSpriteExt(r, 2455, anim6, inst->x + 82.0f, inst->y + 32.0f, -2.0f, 2.0f, 0.0f, blend, armAlphaBody);

    
    int32_t ucon = (AF->ucon >= 0) ? selfInt(inst, AF->ucon) : 0;
    GMLReal arf = (AF->arf >= 0) ? selfReal(inst, AF->arf) : 0.0;
    if (ucon > 0) {
        if (ucon == 1) {
            GMLReal psfx = (AF->psfx >= 0) ? selfReal(inst, AF->psfx) : 0.0;
            lastbeam_casterPlay(ctx, psfx, 0.7, 1.2);  
            arf = 30.0;
            ucon = 2;
        }
        if (ucon == 2) {
            armrot += arf;
            arf -= 2.0;
            if (arf <= 0.0) {
                ucon = 3;
                inst->alarm[10] = 5;
            }
        }
        if (ucon == 4) {
            Instance* gen = Runner_createInstance(runner, inst->x, inst->y, 576);
            if (gen) {
                int32_t typeId = findSelfVarId(ctx->dataWin, "type");
                if (typeId >= 0 && AF->u_gen >= 0) {
                    GMLReal u_gen = selfReal(inst, AF->u_gen);
                    Instance_setSelfVar(gen, typeId, RValue_makeReal(u_gen));
                }
                if (AF->gen >= 0) Instance_setSelfVar(inst, AF->gen, RValue_makeReal((GMLReal)gen->instanceId));
            }
            
            bool inst574Exists = false;
            int32_t nIt = (int32_t)arrlen(runner->instances);
            for (int32_t k = 0; k < nIt; k++) {
                Instance* it = runner->instances[k];
                if (it->active && it->objectIndex == 574) { inst574Exists = true; break; }
            }
            if (!inst574Exists && AF->objHeart >= 0) {
                Instance* hrt = findInstanceByObject(runner, AF->objHeart);
                float hx = hrt ? hrt->x : inst->x;
                float hy = hrt ? hrt->y : inst->y;
                Instance* tgt = Runner_createInstance(runner, hx, hy, 574);
                if (tgt && AF->target >= 0)
                    Instance_setSelfVar(inst, AF->target, RValue_makeReal((GMLReal)tgt->instanceId));
            }
            ucon = 5;
            inst->alarm[10] = 140;
            
            if (AF->u_gen >= 0) {
                GMLReal u_gen = selfReal(inst, AF->u_gen);
                if (u_gen == 2.0) inst->alarm[10] = 130;
            }
            arf = -30.0;
        }
        if (ucon == 6) {
            
            GMLReal genId = (AF->gen >= 0) ? selfReal(inst, AF->gen) : 0.0;
            int32_t nIt = (int32_t)arrlen(runner->instances);
            for (int32_t k = 0; k < nIt; k++) {
                Instance* it = runner->instances[k];
                if (it->active && (int32_t)it->instanceId == (int32_t)genId) {
                    Runner_destroyInstance(runner, it);
                    break;
                }
            }
            armrot += arf;
            arf += 2.0;
            if (arf >= 0.0) {
                ucon = 0;
                if (AF->gMnfight >= 0) globalSet(ctx, AF->gMnfight, RValue_makeReal(3.0));
            }
        }
        if (AF->ucon >= 0) Instance_setSelfVar(inst, AF->ucon, RValue_makeReal((GMLReal)ucon));
    }

    
    {
        bool i577 = false, i574 = false;
        int32_t nIt = (int32_t)arrlen(runner->instances);
        for (int32_t k = 0; k < nIt; k++) {
            Instance* it = runner->instances[k];
            if (!it->active) continue;
            if (it->objectIndex == 577) i577 = true;
            if (it->objectIndex == 574) i574 = true;
        }
        if (!i577 && !i574) {
            
        }
    }

    
    int32_t bcon = (AF->bcon >= 0) ? (int32_t)(selfReal(inst, AF->bcon) * 10.0 + 0.5) : 0;
    
    GMLReal bconReal = (AF->bcon >= 0) ? selfReal(inst, AF->bcon) : 0.0;
    if (bconReal > 0.0) {
        GMLReal r_al = (AF->r_al >= 0) ? selfReal(inst, AF->r_al) : 0.0;
        GMLReal radi = (AF->radi >= 0) ? selfReal(inst, AF->radi) : 0.0;
        GMLReal r_siner = (AF->r_siner >= 0) ? selfReal(inst, AF->r_siner) : 0.0;
        int32_t r_break = (AF->r_break >= 0) ? selfInt(inst, AF->r_break) : 0;
        GMLReal armx = (AF->armx >= 0) ? selfReal(inst, AF->armx) : 0.0;
        GMLReal army = (AF->army >= 0) ? selfReal(inst, AF->army) : 0.0;

        if (bconReal == 1.0) {
            if (AF->ps >= 0) Instance_setSelfVar(inst, AF->ps, RValue_makeReal(0.0));
            inst->alarm[9] = 7;
            r_break = 0;
            r_al = 1.0;
            radi = 0.0;
            r_siner = 0.0;
            arf = 30.0;
            bconReal = 2.0;
        }
        if (bconReal == 2.0) {
            armrot -= arf;
            arf -= 5.0;
            if (arf <= 0.0) {
                bconReal = 3.0;
                inst->alarm[11] = 35;
            }
        }
        if (bconReal == 4.0) {
            bconReal = 4.1;
            inst->alarm[11] = 2;
        }
        if (fabs(bconReal - 4.1) < 1e-6) armrot -= 5.0;
        if (fabs(bconReal - 5.1) < 1e-6) {
            bconReal = 5.0;
            inst->alarm[11] = 5;
        }
        if (bconReal == 5.0) {
            ar_shake = 0.0;
            armrot += 26.0;
        }
        if (bconReal == 6.0) {
            cry = 2;
            ar_shake = 5.0;
            
            double ang = (-armrot - 90.0) * (M_PI / 180.0);
            armx = 150.0 * cos(ang);
            army = -150.0 * sin(ang);
            if (AF->armx >= 0) Instance_setSelfVar(inst, AF->armx, RValue_makeReal(armx));
            if (AF->army >= 0) Instance_setSelfVar(inst, AF->army, RValue_makeReal(army));
            Instance* beam = Runner_createInstance(runner, 320.0, inst->y + 56.0 + army - 20.0, 579);
            if (beam && AF->beam >= 0)
                Instance_setSelfVar(inst, AF->beam, RValue_makeReal((GMLReal)beam->instanceId));
            bconReal = 7.0;
            inst->alarm[11] = 400;
        }

        
        if (bconReal < 7.0 && r_al > 0.0) {
            ar_shake += 0.2;
            if (radi < 60.0) radi += 1.5;
            r_siner += 1.0;
            GMLReal radi_s = sin(r_siner / 2.0) * (radi / 8.0);
            if (AF->radi_s >= 0) Instance_setSelfVar(inst, AF->radi_s, RValue_makeReal(radi_s));
            double ang = (-armrot - 90.0) * (M_PI / 180.0);
            armx = 150.0 * cos(ang);
            army = -150.0 * sin(ang);
            if (AF->armx >= 0) Instance_setSelfVar(inst, AF->armx, RValue_makeReal(armx));
            if (AF->army >= 0) Instance_setSelfVar(inst, AF->army, RValue_makeReal(army));

            if (r_break == 1) {
                radi -= 6.0;
                r_al -= 0.1;
                if (r_al <= 0.0) r_al = 0.0;
            }

            r->drawAlpha = (float)r_al;
            r->drawColor = 0xFFFFFFu;
            int32_t savedPrec = r->circlePrecision;
            r->circlePrecision = 36;
            Renderer_drawCircle(r, inst->x + 56.0f + (float)armx, inst->y + 56.0f + (float)army,
                                (float)(radi + radi_s), true);
            Renderer_drawCircle(r, inst->x + 56.0f + (float)armx, inst->y + 56.0f + (float)army,
                                (float)(radi + radi_s - 1.0), true);
            Renderer_drawSpriteExt(r, 2502, 0,
                                   inst->x + 56.0f + (float)armx, inst->y + 56.0f + (float)army,
                                   (float)(2.0 * (radi + radi_s) / 40.0),
                                   (float)(2.0 * (radi + radi_s) / 40.0),
                                   0.0f, 0xFFFFFFu, (float)r_al);
            Renderer_drawCircle(r, inst->x - 58.0f - (float)armx, inst->y + 56.0f + (float)army,
                                (float)(radi + radi_s), true);
            Renderer_drawCircle(r, inst->x - 58.0f - (float)armx, inst->y + 56.0f + (float)army,
                                (float)(radi + radi_s - 1.0), true);
            Renderer_drawSpriteExt(r, 2502, 0,
                                   inst->x - 58.0f - (float)armx, inst->y + 56.0f + (float)army,
                                   (float)(2.0 * (radi + radi_s) / 40.0),
                                   (float)(2.0 * (radi + radi_s) / 40.0),
                                   0.0f, 0xFFFFFFu, (float)r_al);
            r->drawAlpha = 1.0f;
            r->circlePrecision = savedPrec;
        }

        if (bconReal == 8.0) {
            cry = 0;
            if (AF->gFaceemotion >= 0) globalSet(ctx, AF->gFaceemotion, RValue_makeReal(5.0));
            if (ar_shake > 0.0) ar_shake -= 1.0;
            if (armrot > 0.0) armrot -= 2.0; else armrot = 0.0;
            if (ar_shake <= 0.0) {
                ar_shake = 0.0;
                bconReal = 0.0;
                if (AF->gMnfight >= 0) globalSet(ctx, AF->gMnfight, RValue_makeReal(3.0));
            }
        }

        if (AF->bcon    >= 0) Instance_setSelfVar(inst, AF->bcon,    RValue_makeReal(bconReal));
        if (AF->r_al    >= 0) Instance_setSelfVar(inst, AF->r_al,    RValue_makeReal(r_al));
        if (AF->radi    >= 0) Instance_setSelfVar(inst, AF->radi,    RValue_makeReal(radi));
        if (AF->r_siner >= 0) Instance_setSelfVar(inst, AF->r_siner, RValue_makeReal(r_siner));
        if (AF->r_break >= 0) Instance_setSelfVar(inst, AF->r_break, RValue_makeReal((GMLReal)r_break));
    }
    (void)bcon;

    

    
    int32_t darker = (AF->darker >= 0) ? selfInt(inst, AF->darker) : 0;
    if (darker == 1) {
        
        int32_t nIt = (int32_t)arrlen(runner->instances);
        for (int32_t k = 0; k < nIt; k++) {
            Instance* it = runner->instances[k];
            if (it->active && it->objectIndex == 184) it->depth += 1;
        }
        GMLReal darker_x = (AF->darker_x >= 0) ? selfReal(inst, AF->darker_x) : 0.0;
        if (darker_x < 1.0) darker_x += 0.04;
        r->drawAlpha = (float)darker_x;
        r->drawColor = 0x000000u;
        drawFilledRect(r, -10.0f, -10.0f, 999.0f, 999.0f);
        r->drawAlpha = 1.0f;
        if (AF->darker_x >= 0) Instance_setSelfVar(inst, AF->darker_x, RValue_makeReal(darker_x));
    }

    
    Instance_setSelfVar(inst, AF->anim,  RValue_makeReal(anim));
    Instance_setSelfVar(inst, AF->siner, RValue_makeReal(siner));
    Instance_setSelfVar(inst, AF->side,  RValue_makeReal(side));
    if (AF->ar_shake  >= 0) Instance_setSelfVar(inst, AF->ar_shake,  RValue_makeReal(ar_shake));
    if (AF->armrot    >= 0) Instance_setSelfVar(inst, AF->armrot,    RValue_makeReal(armrot));
    if (AF->arf       >= 0) Instance_setSelfVar(inst, AF->arf,       RValue_makeReal(arf));
    if (AF->cry       >= 0) Instance_setSelfVar(inst, AF->cry,       RValue_makeReal((GMLReal)cry));
    #undef AF
}



















static struct {
    int32_t con, facey, facescale, siner;
    int32_t gl, gc;
    int32_t bb, cc, dd;
    int32_t a_v, b_v, c_v, d_v;     
    int32_t c_counter, rad;
    int32_t pd, ldrx, ldry;         
    int32_t gonercon;               
    int32_t objHeart, objAsrielBody;
    bool ready;
} hgBodyCache = { .ready = false };

static void initHgBodyCache(DataWin* dw) {
    hgBodyCache.con       = findSelfVarId(dw, "con");
    hgBodyCache.facey     = findSelfVarId(dw, "facey");
    hgBodyCache.facescale = findSelfVarId(dw, "facescale");
    hgBodyCache.siner     = findSelfVarId(dw, "siner");
    hgBodyCache.gl        = findSelfVarId(dw, "gl");
    hgBodyCache.gc        = findSelfVarId(dw, "gc");
    hgBodyCache.bb        = findSelfVarId(dw, "bb");
    hgBodyCache.cc        = findSelfVarId(dw, "cc");
    hgBodyCache.dd        = findSelfVarId(dw, "dd");
    hgBodyCache.a_v       = findSelfVarId(dw, "a");
    hgBodyCache.b_v       = findSelfVarId(dw, "b");
    hgBodyCache.c_v       = findSelfVarId(dw, "c");
    hgBodyCache.d_v       = findSelfVarId(dw, "d");
    hgBodyCache.c_counter = findSelfVarId(dw, "c_counter");
    hgBodyCache.rad       = findSelfVarId(dw, "rad");
    hgBodyCache.pd        = findSelfVarId(dw, "pd");
    hgBodyCache.ldrx      = findSelfVarId(dw, "ldrx");
    hgBodyCache.ldry      = findSelfVarId(dw, "ldry");
    hgBodyCache.gonercon  = findSelfVarId(dw, "gonercon");
    hgBodyCache.objHeart       = findObjectIndex(dw, "obj_heart");
    hgBodyCache.objAsrielBody  = findObjectIndex(dw, "obj_asriel_body");
    hgBodyCache.ready = (hgBodyCache.con >= 0 && hgBodyCache.facey >= 0 &&
                         hgBodyCache.facescale >= 0 && hgBodyCache.siner >= 0 &&
                         hgBodyCache.c_counter >= 0);
}


static void hg_casterFree(VMContext* ctx, GMLReal snd) {
    if (snd == -3.0) {
        BuiltinFunc asa = VMBuiltins_find("audio_stop_all");
        if (asa) { RValue r = asa(ctx, NULL, 0); RValue_free(&r); }
    } else {
        BuiltinFunc ass = VMBuiltins_find("audio_stop_sound");
        if (ass) { RValue a[1] = { RValue_makeReal(snd) }; RValue r = ass(ctx, a, 1); RValue_free(&r); }
    }
}

static void native_hgBody_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!hgBodyCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;
    DataWin* dw = ctx->dataWin;
    float rw = (runner->currentRoom ? (float)runner->currentRoom->width  : 640.0f);
    float rh = (runner->currentRoom ? (float)runner->currentRoom->height : 480.0f);

    GMLReal con = selfReal(inst, hgBodyCache.con);

    
    if (con == -1.0) {
        inst->imageAlpha += 0.05f;
        if (inst->imageAlpha >= 1.0f) {
            con = 0.1;
            inst->alarm[4] = 20;
        }
    }

    GMLReal facey     = selfReal(inst, hgBodyCache.facey);
    GMLReal facescale = selfReal(inst, hgBodyCache.facescale);

    
    if (con < 3.0) {
        Renderer_drawSpriteExt(r, 2510, 0, inst->x,            inst->y + (float)(facey / 6.0),       2.0f, 2.0f,                     0.0f, 0xFFFFFFu, inst->imageAlpha);
        Renderer_drawSpriteExt(r, 2508, 0, inst->x,            inst->y - (float)(facey / 2.0),       2.0f, 2.0f,                     0.0f, 0xFFFFFFu, inst->imageAlpha);
        Renderer_drawSpriteExt(r, 2511, 0, inst->x + 88.0f,    inst->y + 72.0f + (float)facey,       2.0f, 2.0f + (float)facescale,  0.0f, 0xFFFFFFu, inst->imageAlpha);
        Renderer_drawSpriteExt(r, 2509, 0, inst->x + 104.0f,   (inst->y + 248.0f) - (float)(facey / 2.0), 2.0f, 2.0f,                 0.0f, 0xFFFFFFu, inst->imageAlpha);
    }

    
    if (fabs(con - 1.1) < 1e-6) {
        GMLReal gl = (hgBodyCache.gl >= 0) ? selfReal(inst, hgBodyCache.gl) : 0.0;
        lastbeam_casterPlay(ctx, gl, 0.8, 1.0);
        con = 1.0;
    }

    
    if (con == 1.0) {
        facey     -= 3.5;
        facescale -= 0.2;
        if (hgBodyCache.siner >= 0) Instance_setSelfVar(inst, hgBodyCache.siner, RValue_makeReal(0.0));
        if (facescale < -1.0) {
            con = 1.9;
            inst->alarm[4] = 75;
            inst->alarm[6] = 100;
        }
    }

    GMLReal siner = selfReal(inst, hgBodyCache.siner);

    
    if (fabs(con - 1.9) < 1e-6) {
        siner += 1.0;
        facey     += (GMLReal)(sin(siner / 1.5) * 8.0);
        facescale += (GMLReal)(sin(siner / 1.5) * 0.2);
    }

    
    if (fabs(con - 2.9) < 1e-6) {
        GMLReal gc = (hgBodyCache.gc >= 0) ? selfReal(inst, hgBodyCache.gc) : 0.0;
        lastbeam_casterPlay(ctx, gc, 1.0, 1.0);
        con = 3.0;
    }

    
    GMLReal c_counter = selfReal(inst, hgBodyCache.c_counter);
    GMLReal rad       = (hgBodyCache.rad >= 0) ? selfReal(inst, hgBodyCache.rad) : 0.0;
    GMLReal cc_local  = (hgBodyCache.cc >= 0)  ? selfReal(inst, hgBodyCache.cc)  : 1.0;
    GMLReal bb_local  = (hgBodyCache.bb >= 0)  ? selfReal(inst, hgBodyCache.bb)  : 12.0;
    GMLReal dd_local  = (hgBodyCache.dd >= 0)  ? selfReal(inst, hgBodyCache.dd)  : 8.0;

    if (con == 3.0) {
        if (cc_local < 80.0) cc_local += 0.5;
        
        inst->spriteIndex = 2512;
        if (inst->imageAlpha > 0.14f) inst->imageAlpha -= 0.02f;

        
        
        if (hgBodyCache.b_v >= 0) Instance_setSelfVar(inst, hgBodyCache.b_v, RValue_makeReal(bb_local));
        if (hgBodyCache.c_v >= 0) Instance_setSelfVar(inst, hgBodyCache.c_v, RValue_makeReal(cc_local));
        if (hgBodyCache.d_v >= 0) Instance_setSelfVar(inst, hgBodyCache.d_v, RValue_makeReal(dd_local));
        GMLReal a_val = (hgBodyCache.a_v >= 0) ? selfReal(inst, hgBodyCache.a_v) + 1.0 : 1.0;
        int32_t sprIdx = inst->spriteIndex;
        int32_t subimg = (int32_t)inst->imageIndex;
        if (sprIdx >= 0 && (uint32_t)sprIdx < dw->sprt.count) {
            Sprite* spr = &dw->sprt.sprites[sprIdx];
            int32_t sh = (int32_t)spr->height;
            int32_t sw = (int32_t)spr->width;
            float alpha = inst->imageAlpha;
            for (int32_t i = 0; i < sh; i++) {
                a_val += 1.0;
                int32_t srcH = (int32_t)(sin(a_val) * dd_local);
                if (srcH <= 0) continue;
                float dx = (float)inst->x + (float)(sin(a_val / bb_local) * cc_local);
                float dy = (float)inst->y + (float)(i * 2);
                Renderer_drawSpritePartExt(r, sprIdx, subimg, 0, i, sw, srcH,
                                           dx, dy, 2.0f, 2.0f, 0xFFFFFFu, alpha);
            }
        }
        if (hgBodyCache.a_v >= 0) Instance_setSelfVar(inst, hgBodyCache.a_v, RValue_makeReal(a_val));

        
        r->drawColor = 0xFFFFFFu;
        r->drawAlpha = 1.0f - inst->imageAlpha;
        BuiltinFunc rnd = VMBuiltins_find("random");
        if (rnd) {
            float midX = rw / 2.0f;
            float midY = rh / 2.0f;
            
            #define CALL_RND(outVar, mx) do {                      \
                RValue __a = RValue_makeReal(mx);                  \
                RValue __v = rnd(ctx, &__a, 1);                    \
                outVar = RValue_toReal(__v);                       \
                RValue_free(&__v);                                 \
            } while (0)
            for (int32_t g = 0; g < 4; g++) {
                
                
                
                
                for (int32_t i = 0; i < 5; i++) {
                    double r1, r2, r3;
                    CALL_RND(r1, 10.0);
                    CALL_RND(r2, 10.0);
                    float sx = midX + (float)(r1 - r2);
                    CALL_RND(r1, 10.0);
                    CALL_RND(r2, 10.0);
                    float sy = midY + (float)(r1 - r2);
                    float ex, ey;
                    if (g == 0)      { CALL_RND(r3, rw); ex = (float)r3; ey = rh;   }
                    else if (g == 1) { CALL_RND(r3, rw); ex = (float)r3; ey = 0.0f; }
                    else if (g == 2) { CALL_RND(r3, rh); ex = 0.0f;      ey = (float)r3; }
                    else             { CALL_RND(r3, rh); ex = rw;        ey = (float)r3; }
                    r->vtable->drawLineColor(r, sx, sy, ex, ey, 2.0f,
                                             0xFFFFFFu, 8421504u, r->drawAlpha);
                }
            }
            #undef CALL_RND
        }
        r->drawAlpha = 1.0f;

        
        rad = (c_counter - 180.0) / 1.5;
        if (rad < 20.0) rad = 20.0;
        int32_t savedPrec = r->circlePrecision;
        r->circlePrecision = 16;
        r->drawColor = 32768u; 
        Renderer_drawCircle(r, 320.0f, 240.0f, (float)rad, true);

        
        if (c_counter < 295.0) {
            BuiltinFunc cc_b = VMBuiltins_find("collision_circle");
            if (cc_b) {
                RValue args[6] = {
                    RValue_makeReal(320.0), RValue_makeReal(240.0),
                    RValue_makeReal(rad - 5.0), RValue_makeReal(744.0),
                    RValue_makeReal(0.0), RValue_makeReal(1.0)
                };
                RValue res = cc_b(ctx, args, 6);
                if (RValue_toInt32(res) > 0) {
                    Runner_executeEvent(runner, inst, 7, 17); 
                }
                RValue_free(&res);
            } else {
                
                
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t k = 0; k < n; k++) {
                    Instance* it = runner->instances[k];
                    if (!it->active || it->objectIndex != 744) continue;
                    float dx = it->x + 8.0f - 320.0f; 
                    float dy = it->y + 8.0f - 240.0f;
                    if ((dx * dx + dy * dy) < ((rad - 5.0) * (rad - 5.0))) {
                        Runner_executeEvent(runner, inst, 7, 17);
                        break;
                    }
                }
            }
        }

        
        GMLReal pullSpeed = (c_counter < 180.0) ? 1.0 : ((c_counter > 180.0) ? 2.0 : 0.0);
        if (pullSpeed > 0.0) {
            int32_t n = (int32_t)arrlen(runner->instances);
            for (int32_t k = 0; k < n; k++) {
                Instance* it = runner->instances[k];
                if (!it->active || it->objectIndex != 744) continue;
                double dx = 312.0 - it->x;
                double dy = 232.0 - it->y;
                double pd = atan2(-dy, dx) * (180.0 / M_PI);
                double rad_pd = pd * (M_PI / 180.0);
                double ldrx = pullSpeed * cos(rad_pd);
                double ldry = -pullSpeed * sin(rad_pd);
                it->x += (float)ldrx;
                it->y += (float)ldry;
            }
        }

        c_counter += 1.0;

        
        if (c_counter > 180.0) {
            r->drawColor = 0xFFFFFFu;
            r->drawAlpha = (float)((c_counter - 180.0) / 60.0);
            Renderer_drawCircle(r, rw / 2.0f, rh / 2.0f, (float)((c_counter - 180.0) / 1.5), false);
            r->drawAlpha = (float)((c_counter - 210.0) / 80.0);
            drawFilledRect(r, -10.0f, -10.0f, 999.0f, 999.0f);
            r->drawAlpha = 1.0f;

            
            if (c_counter > 275.0 && hgBodyCache.objHeart >= 0) {
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t k = 0; k < n; k++) {
                    Instance* it = runner->instances[k];
                    if (it->active && it->objectIndex == hgBodyCache.objHeart) {
                        it->imageAlpha -= 0.05f;
                    }
                }
            }
            
            if (c_counter > 320.0) {
                GMLReal gl = (hgBodyCache.gl >= 0) ? selfReal(inst, hgBodyCache.gl) : 0.0;
                GMLReal gc = (hgBodyCache.gc >= 0) ? selfReal(inst, hgBodyCache.gc) : 0.0;
                hg_casterFree(ctx, gl);
                hg_casterFree(ctx, gc);
                
                bool has570 = false;
                int32_t n = (int32_t)arrlen(runner->instances);
                for (int32_t k = 0; k < n; k++) {
                    Instance* it = runner->instances[k];
                    if (it->active && it->objectIndex == 570) { has570 = true; break; }
                }
                if (has570) {
                    if (hgBodyCache.objAsrielBody >= 0 && hgBodyCache.gonercon >= 0) {
                        for (int32_t k = 0; k < n; k++) {
                            Instance* it = runner->instances[k];
                            if (it->active && it->objectIndex == hgBodyCache.objAsrielBody) {
                                Instance_setSelfVar(it, hgBodyCache.gonercon, RValue_makeReal(10.0));
                            }
                        }
                    }
                    Runner_destroyInstance(runner, inst);
                    r->circlePrecision = savedPrec;
                    return;
                }
            }
        }

        r->circlePrecision = savedPrec;
    }

    
    if (hgBodyCache.objHeart >= 0) {
        int32_t n = (int32_t)arrlen(runner->instances);
        for (int32_t k = 0; k < n; k++) {
            Instance* it = runner->instances[k];
            if (!it->active || it->objectIndex != hgBodyCache.objHeart) continue;
            if (it->x < 0.0f)               it->x = 0.0f;
            if (it->x > rw - 16.0f)         it->x = rw - 16.0f;
            if (it->y < 0.0f)               it->y = 0.0f;
            if (it->y > rh - 16.0f)         it->y = rh - 16.0f;
        }
    }

    
    Instance_setSelfVar(inst, hgBodyCache.con,       RValue_makeReal(con));
    Instance_setSelfVar(inst, hgBodyCache.facey,     RValue_makeReal(facey));
    Instance_setSelfVar(inst, hgBodyCache.facescale, RValue_makeReal(facescale));
    Instance_setSelfVar(inst, hgBodyCache.siner,     RValue_makeReal(siner));
    if (hgBodyCache.c_counter >= 0) Instance_setSelfVar(inst, hgBodyCache.c_counter, RValue_makeReal(c_counter));
    if (hgBodyCache.rad       >= 0) Instance_setSelfVar(inst, hgBodyCache.rad,       RValue_makeReal(rad));
    if (hgBodyCache.cc        >= 0) Instance_setSelfVar(inst, hgBodyCache.cc,        RValue_makeReal(cc_local));
}









static void native_normalplink_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    inst->imageAlpha -= 0.08f;
    if (inst->imageAlpha < 0.09f) {
        Runner_destroyInstance(runner, inst);
    }
}















static struct {
    int32_t ap;
    bool ready;
} glowparticle1Cache = { .ready = false };

static void initGlowparticle1Cache(DataWin* dw) {
    glowparticle1Cache.ap    = findSelfVarId(dw, "ap");
    glowparticle1Cache.ready = (glowparticle1Cache.ap >= 0);
}



static inline double nativeRandomFast(double n) {
    return ((double)rand() / (double)RAND_MAX) * n;
}

static void native_glowparticle1_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!glowparticle1Cache.ready) return;

    
    
    
    int32_t ap = selfInt(inst, glowparticle1Cache.ap);
    if (ap == 0) {
        inst->imageAlpha += 0.25f;
        if (inst->imageAlpha > 0.6f) {
            Instance_setSelfVar(inst, glowparticle1Cache.ap, RValue_makeReal(1.0));
            
        }
    }

    
    
    
    
    float randScale = 1.0f / (float)RAND_MAX;
    inst->direction += (float)rand() * randScale * 6.0f    - 3.0f;
    inst->speed     += (float)rand() * randScale * 0.04f   - 0.02f;

    
    
    
    
    
    
    if (inst->speed != 0.0f) {
        float radDir = inst->direction * (float)(M_PI / 180.0);
        inst->hspeed =  inst->speed * cosf(radDir);
        inst->vspeed = -inst->speed * sinf(radDir);
    } else {
        inst->hspeed = 0.0f;
        inst->vspeed = 0.0f;
    }

    inst->imageAlpha -= 0.01f;
    if (inst->imageAlpha < 0.02f) {
        Runner_destroyInstance(runner, inst);
    }
}

















static struct {
    int32_t goal, dont;
    bool ready;
} normaldropCache = { .ready = false };

static void initNormaldropCache(DataWin* dw) {
    normaldropCache.goal = findSelfVarId(dw, "goal");
    normaldropCache.dont = findSelfVarId(dw, "dont");
    normaldropCache.ready = (normaldropCache.goal >= 0);
}

static void native_normaldrop_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!normaldropCache.ready) return;

    int32_t roomIdx = runner->currentRoomIndex;

    
    
    
    float goal;
    if (roomIdx == 107) {
        goal = (float)selfReal(inst, normaldropCache.goal);
    } else {
        goal = inst->ystart + 240.0f;
    }

    if (inst->y > goal) {
        
        
        float cx = inst->x;
        float cy = inst->y + 5.0f;
        Runner_createInstance(runner, cx, cy, 1153);
        Runner_createInstance(runner, cx, cy, 1153);
        Runner_createInstance(runner, cx, cy, 1153);
        Runner_destroyInstance(runner, inst);
        return;
    }

    
    
    
    bool needDontRead = (roomIdx == 109);
    int32_t dont = 0;
    if (needDontRead && normaldropCache.dont >= 0) {
        dont = selfInt(inst, normaldropCache.dont);
    }

    if (dont == 0 && runner->currentRoom != NULL) {
        float viewX = (float)runner->currentRoom->views[runner->viewCurrent].viewX;
        if (inst->x < viewX - 40.0f)  inst->x += 361.0f;
        if (inst->x > viewX + 360.0f) inst->x -= 361.0f;
    }
}







static void native_normaldrop_Other11(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    float cx = inst->x;
    float cy = inst->y + 5.0f;
    Runner_createInstance(runner, cx, cy, 1153);
    Runner_createInstance(runner, cx, cy, 1153);
    Runner_createInstance(runner, cx, cy, 1153);
    Runner_destroyInstance(runner, inst);
}







static struct {
    int32_t topy, bottomy, active, speeded, phase, d_v;
    int32_t colliding;         
    int32_t gInteract;
    int32_t objMainchara;      
    int32_t objWaterpushrockgen;
    int32_t objTime;
    int32_t obj1138;           
    int32_t upId, downId;      
    bool ready;
} waterpushrockCache = { .ready = false };

static void initWaterpushrockCache(VMContext* ctx, DataWin* dw) {
    waterpushrockCache.topy      = findSelfVarId(dw, "topy");
    waterpushrockCache.bottomy   = findSelfVarId(dw, "bottomy");
    waterpushrockCache.active    = findSelfVarId(dw, "active");
    waterpushrockCache.speeded   = findSelfVarId(dw, "speeded");
    waterpushrockCache.phase     = findSelfVarId(dw, "phase");
    waterpushrockCache.d_v       = findSelfVarId(dw, "d");
    waterpushrockCache.colliding = findSelfVarId(dw, "colliding");
    waterpushrockCache.gInteract = findGlobalVarId(ctx, "interact");
    waterpushrockCache.objMainchara       = findObjectIndex(dw, "obj_mainchara");
    waterpushrockCache.objWaterpushrockgen = findObjectIndex(dw, "obj_waterpushrockgen");
    waterpushrockCache.objTime            = findObjectIndex(dw, "obj_time");
    waterpushrockCache.obj1138            = 1138;
    waterpushrockCache.upId   = findSelfVarId(dw, "up");
    waterpushrockCache.downId = findSelfVarId(dw, "down");
    waterpushrockCache.ready = (waterpushrockCache.topy >= 0 && waterpushrockCache.bottomy >= 0 &&
                                waterpushrockCache.active >= 0 && waterpushrockCache.speeded >= 0 &&
                                waterpushrockCache.phase >= 0);
}

static void native_waterpushrock_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!waterpushrockCache.ready) return;
    DataWin* dw = ctx->dataWin;

    
    int32_t sprH = 0;
    if (inst->spriteIndex >= 0 && (uint32_t)inst->spriteIndex < dw->sprt.count) {
        sprH = (int32_t)dw->sprt.sprites[inst->spriteIndex].height;
    }
    inst->depth = 50000 - ((int32_t)(inst->y * 10.0f) + sprH * 10);

    
    GMLReal gInteract = (waterpushrockCache.gInteract >= 0) ? globalReal(ctx, waterpushrockCache.gInteract) : 0.0;
    if (gInteract != 5.0) inst->vspeed = 8.0f;
    else                  inst->vspeed = 0.0f;

    
    GMLReal topy = selfReal(inst, waterpushrockCache.topy);
    if ((GMLReal)inst->y > topy) {
        int32_t speeded = selfInt(inst, waterpushrockCache.speeded);
        if (speeded == 0) {
            Instance_setSelfVar(inst, waterpushrockCache.phase,   RValue_makeReal(0.0));
            Instance_setSelfVar(inst, waterpushrockCache.speeded, RValue_makeReal(1.0));
        }
        if (gInteract != 5.0) inst->vspeed = 4.0f;
    }
    Instance_computeSpeedFromComponents(inst);

    
    int32_t active = selfInt(inst, waterpushrockCache.active);
    if (active == 1 && gInteract != 5.0) {
        
        bool has1138 = false;
        int32_t n = (int32_t)arrlen(runner->instances);
        for (int32_t i = 0; i < n; i++) {
            Instance* it = runner->instances[i];
            if (it->active && it->objectIndex == waterpushrockCache.obj1138) { has1138 = true; break; }
        }
        if (has1138) {
            
            Instance* gen = (waterpushrockCache.objWaterpushrockgen >= 0)
                          ? findInstanceByObject(runner, waterpushrockCache.objWaterpushrockgen) : NULL;
            int32_t colliding = (gen && waterpushrockCache.colliding >= 0)
                              ? (int32_t)RValue_toReal(Instance_getSelfVar(gen, waterpushrockCache.colliding)) : 0;
            if (colliding == 0) {
                
                InstanceBBox bb = Collision_computeBBox(dw, inst);
                if (bb.valid && waterpushrockCache.objMainchara >= 0) {
                    float rx1 = bb.left;
                    float ry1 = bb.bottom + 1.0f;
                    float rx2 = bb.right;
                    float ry2 = bb.bottom + inst->vspeed + 1.0f;
                    if (ry1 > ry2) { float t = ry1; ry1 = ry2; ry2 = t; }
                    Instance* hit = NULL;
                    for (int32_t i = 0; i < n; i++) {
                        Instance* mc = runner->instances[i];
                        if (!mc->active || mc == inst) continue;
                        if (!Collision_matchesTarget(dw, mc, waterpushrockCache.objMainchara)) continue;
                        InstanceBBox mb = Collision_computeBBox(dw, mc);
                        if (!mb.valid) continue;
                        if (!(rx1 >= mb.right || mb.left >= rx2 || ry1 >= mb.bottom || mb.top >= ry2)) {
                            hit = mc; break;
                        }
                    }
                    if (hit != NULL) {
                        hit->y += inst->vspeed;
                        
                        if (waterpushrockCache.objTime >= 0) {
                            Instance* ot = findInstanceByObject(runner, waterpushrockCache.objTime);
                            if (ot) {
                                int32_t up   = (waterpushrockCache.upId   >= 0) ? (int32_t)RValue_toReal(Instance_getSelfVar(ot, waterpushrockCache.upId))   : 0;
                                int32_t down = (waterpushrockCache.downId >= 0) ? (int32_t)RValue_toReal(Instance_getSelfVar(ot, waterpushrockCache.downId)) : 0;
                                if (up)   hit->y += 3.0f;
                                if (down && inst->vspeed > 3.0f) hit->y -= 3.0f;
                            }
                        }
                        hit->x = roundf(hit->x);
                        hit->y = roundf(hit->y);
                        if (gen && waterpushrockCache.colliding >= 0)
                            Instance_setSelfVar(gen, waterpushrockCache.colliding, RValue_makeReal(1.0));
                    }
                }
            }
        }
    }

    
    GMLReal bottomy = selfReal(inst, waterpushrockCache.bottomy);
    if ((GMLReal)inst->y > bottomy) {
        Instance* splash = Runner_createInstance(runner, inst->x, inst->y, 1140);
        if (splash && runner->currentRoomIndex == 91) {
            splash->alarm[0] = 2;
        }
        if (waterpushrockCache.d_v >= 0 && splash)
            Instance_setSelfVar(inst, waterpushrockCache.d_v, RValue_makeReal((GMLReal)splash->instanceId));
        Runner_destroyInstance(runner, inst);
        return;
    }
    
    if ((GMLReal)inst->y > 350.0) {
        Runner_destroyInstance(runner, inst);
    }
}










static struct {
    int32_t won, using_v;
    int32_t objWboardTile;  
    bool ready;
} waterboardpuzzle1Cache = { .ready = false };

static void initWaterboardpuzzle1Cache(DataWin* dw) {
    waterboardpuzzle1Cache.won     = findSelfVarId(dw, "won");
    waterboardpuzzle1Cache.using_v = findSelfVarId(dw, "using");
    waterboardpuzzle1Cache.objWboardTile = 1116;
    waterboardpuzzle1Cache.ready = (waterboardpuzzle1Cache.won >= 0 &&
                                    waterboardpuzzle1Cache.using_v >= 0);
}



typedef struct {
    Instance* inst;
    float left, top, right, bottom;  
    int32_t using_val;                
} WboardTileCache;


static inline WboardTileCache* wboard_findAt(WboardTileCache* tiles, int32_t tc,
                                             float px, float py) {
    for (int32_t i = 0; i < tc; i++) {
        WboardTileCache* t = &tiles[i];
        if (px >= t->left && px < t->right && py >= t->top && py < t->bottom) return t;
    }
    return NULL;
}

static void native_waterboardpuzzle1_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!waterboardpuzzle1Cache.ready) return;
    DataWin* dw = ctx->dataWin;

    int32_t won = selfInt(inst, waterboardpuzzle1Cache.won);
    if (won != 0) return;

    int32_t objTile = waterboardpuzzle1Cache.objWboardTile;
    int32_t usingId = waterboardpuzzle1Cache.using_v;

    #define WBOARD_MAX_TILES 64
    WboardTileCache tiles[WBOARD_MAX_TILES];
    int32_t tileCount = 0;
    int32_t n = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < n && tileCount < WBOARD_MAX_TILES; i++) {
        Instance* it = runner->instances[i];
        if (!it->active || it->objectIndex != objTile) continue;
        InstanceBBox bb = Collision_computeBBox(dw, it);
        if (!bb.valid) continue;
        tiles[tileCount].inst      = it;
        tiles[tileCount].left      = (float)bb.left;
        tiles[tileCount].top       = (float)bb.top;
        tiles[tileCount].right     = (float)bb.right;
        tiles[tileCount].bottom    = (float)bb.bottom;
        tiles[tileCount].using_val = (int32_t)RValue_toReal(Instance_getSelfVar(it, usingId));
        tileCount++;
    }

    if (tileCount == 0) return;

    
    for (int32_t ii = 0; ii < tileCount; ii++) {
        WboardTileCache* me = &tiles[ii];
        float xx = me->inst->x;
        float yy = me->inst->y;

        
        WboardTileCache* larr[4] = { NULL, NULL, NULL, NULL };
        WboardTileCache* rarr[4] = { NULL, NULL, NULL, NULL };
        WboardTileCache* uarr[4] = { NULL, NULL, NULL, NULL };
        WboardTileCache* darr[4] = { NULL, NULL, NULL, NULL };
        int32_t lx[4] = { 1, 0, 0, 0 };
        int32_t rx[4] = { 1, 0, 0, 0 };
        int32_t ux[4] = { 1, 0, 0, 0 };
        int32_t dx[4] = { 1, 0, 0, 0 };

        int32_t h = 0, v = 0;
        int32_t doodly = 0;
        for (int32_t j = 1; j <= 3; j++) {
            larr[j] = wboard_findAt(tiles, tileCount, xx -  2.0f - (float)doodly, yy);
            rarr[j] = wboard_findAt(tiles, tileCount, xx + 22.0f + (float)doodly, yy);
            uarr[j] = wboard_findAt(tiles, tileCount, xx, yy -  2.0f - (float)doodly);
            darr[j] = wboard_findAt(tiles, tileCount, xx, yy + 22.0f + (float)doodly);

            if (larr[j] != NULL && lx[j-1] == 1 && larr[j]->using_val == 3) { h += 1; lx[j] = 1; }
            if (rarr[j] != NULL && rx[j-1] == 1 && rarr[j]->using_val == 3) { h += 1; rx[j] = 1; }
            if (uarr[j] != NULL && ux[j-1] == 1 && uarr[j]->using_val == 3) { v += 1; ux[j] = 1; }
            if (darr[j] != NULL && dx[j-1] == 1 && darr[j]->using_val == 3) { v += 1; dx[j] = 1; }

            doodly += 20;
        }

        if (v >= 3 && me->using_val == 3) {
            won = 90;
            Instance_setSelfVar(inst, waterboardpuzzle1Cache.won, RValue_makeReal(90.0));
            Instance_setSelfVar(me->inst, usingId, RValue_makeReal(5.0));
            me->using_val = 5;
            for (int32_t nn = 1; nn <= 3; nn++) {
                if (uarr[nn] != NULL && ux[nn-1] == 1) { Instance_setSelfVar(uarr[nn]->inst, usingId, RValue_makeReal(5.0)); uarr[nn]->using_val = 5; }
                if (darr[nn] != NULL && dx[nn-1] == 1) { Instance_setSelfVar(darr[nn]->inst, usingId, RValue_makeReal(5.0)); darr[nn]->using_val = 5; }
            }
        }
        if (h >= 3 && me->using_val == 3) {
            won = 90;
            Instance_setSelfVar(inst, waterboardpuzzle1Cache.won, RValue_makeReal(90.0));
            Instance_setSelfVar(me->inst, usingId, RValue_makeReal(5.0));
            me->using_val = 5;
            for (int32_t nn = 1; nn <= 3; nn++) {
                if (rarr[nn] != NULL && rx[nn-1] == 1) { Instance_setSelfVar(rarr[nn]->inst, usingId, RValue_makeReal(5.0)); rarr[nn]->using_val = 5; }
                if (larr[nn] != NULL && lx[nn-1] == 1) { Instance_setSelfVar(larr[nn]->inst, usingId, RValue_makeReal(5.0)); larr[nn]->using_val = 5; }
            }
        }
    }
    #undef WBOARD_MAX_TILES
}





static struct {
    int32_t f_test, myview, myview_b, g_heart, gg, xhome, scrollspeed;
    int32_t objMainchara;
    bool ready;
} waterstarBgCache = { .ready = false };

static void initWaterstarBgCache(DataWin* dw) {
    waterstarBgCache.f_test       = findSelfVarId(dw, "f_test");
    waterstarBgCache.myview       = findSelfVarId(dw, "myview");
    waterstarBgCache.myview_b     = findSelfVarId(dw, "myview_b");
    waterstarBgCache.g_heart      = findSelfVarId(dw, "g_heart");
    waterstarBgCache.gg           = findSelfVarId(dw, "gg");
    waterstarBgCache.xhome        = findSelfVarId(dw, "xhome");
    waterstarBgCache.scrollspeed  = findSelfVarId(dw, "scrollspeed");
    waterstarBgCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    waterstarBgCache.ready = (waterstarBgCache.f_test >= 0 && waterstarBgCache.xhome >= 0 &&
                              waterstarBgCache.scrollspeed >= 0);
}

static void native_waterstarBg_Other10(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!waterstarBgCache.ready || runner->currentRoom == NULL) return;

    int32_t f_test = selfInt(inst, waterstarBgCache.f_test);
    if (f_test != 1) return;

    int32_t viewIdx = runner->viewCurrent;
    float viewX = (float)runner->currentRoom->views[viewIdx].viewX;
    float viewW = (float)runner->currentRoom->views[viewIdx].viewWidth;
    float roomW = (float)runner->currentRoom->width;

    float myview = viewX;
    float myview_b = viewX;

    int32_t g_heart = selfInt(inst, waterstarBgCache.g_heart);
    if (g_heart < 4 && waterstarBgCache.objMainchara >= 0) {
        Instance* mc = findInstanceByObject(runner, waterstarBgCache.objMainchara);
        if (mc && mc->x > roomW - 160.0f) {
            myview -= 140.0f;
        }
    }
    g_heart += 1;
    float gg = roomW - viewW;

    if (myview < 0.0f) myview = 0.0f;

    float xhome = (float)selfReal(inst, waterstarBgCache.xhome);

    if (inst->x < myview - 20.0f && myview < gg) {
        inst->x += 350.0f;
        xhome   += 350.0f;
    }
    if (inst->x > myview + 340.0f) {
        inst->x -= 350.0f;
        xhome   -= 350.0f;
    }

    GMLReal scrollspeed = selfReal(inst, waterstarBgCache.scrollspeed);
    if (viewX >= 0.0f) {
        inst->x = xhome + (float)floor(viewX - (viewX * (float)scrollspeed) + 0.5);
    }
    if (viewX >= gg) {
        inst->x = xhome + (float)floor(gg - (gg * (float)scrollspeed) + 0.5);
    }

    Instance_setSelfVar(inst, waterstarBgCache.g_heart, RValue_makeReal((GMLReal)g_heart));
    Instance_setSelfVar(inst, waterstarBgCache.xhome,   RValue_makeReal((GMLReal)xhome));
    if (waterstarBgCache.myview   >= 0) Instance_setSelfVar(inst, waterstarBgCache.myview,   RValue_makeReal((GMLReal)myview));
    if (waterstarBgCache.myview_b >= 0) Instance_setSelfVar(inst, waterstarBgCache.myview_b, RValue_makeReal((GMLReal)myview_b));
    if (waterstarBgCache.gg       >= 0) Instance_setSelfVar(inst, waterstarBgCache.gg,       RValue_makeReal((GMLReal)gg));
}


















#define SPEARTILE_MAX_VARIDS 512
static struct {
    int32_t conCandidates[SPEARTILE_MAX_VARIDS];    int32_t conCount;
    int32_t facerCandidates[SPEARTILE_MAX_VARIDS];  int32_t facerCount;
    int32_t soundedCandidates[SPEARTILE_MAX_VARIDS];int32_t soundedCount;
    int32_t activeCandidates[SPEARTILE_MAX_VARIDS]; int32_t activeCount;
    int32_t dutyCandidates[SPEARTILE_MAX_VARIDS];   int32_t dutyCount;
    int32_t spearbud;  
    int32_t sound2;    
    int32_t up_v, down_v, left_v, right_v;
    int32_t objSpeartileWall;   
    int32_t objObstacle1575;    
    int32_t objMainchara;       
    int32_t objSoundExists;     
    int32_t objSpeartilegen;
    int32_t objTime;
    bool ready;
} speartileCache = { .ready = false };

static void initSpeartileCache(DataWin* dw) {
    speartileCache.conCount     = findAllSelfVarIds(dw, "con",     speartileCache.conCandidates,     SPEARTILE_MAX_VARIDS);
    speartileCache.facerCount   = findAllSelfVarIds(dw, "facer",   speartileCache.facerCandidates,   SPEARTILE_MAX_VARIDS);
    speartileCache.soundedCount = findAllSelfVarIds(dw, "sounded", speartileCache.soundedCandidates, SPEARTILE_MAX_VARIDS);
    speartileCache.activeCount  = findAllSelfVarIds(dw, "active",  speartileCache.activeCandidates,  SPEARTILE_MAX_VARIDS);
    speartileCache.dutyCount    = findAllSelfVarIds(dw, "duty",    speartileCache.dutyCandidates,    SPEARTILE_MAX_VARIDS);
    speartileCache.spearbud = findSelfVarId(dw, "spearbud");
    speartileCache.sound2   = findSelfVarId(dw, "sound2");
    speartileCache.up_v     = findSelfVarId(dw, "up");
    speartileCache.down_v   = findSelfVarId(dw, "down");
    speartileCache.left_v   = findSelfVarId(dw, "left");
    speartileCache.right_v  = findSelfVarId(dw, "right");
    speartileCache.objSpeartileWall = 1043;
    speartileCache.objObstacle1575  = 1575;
    speartileCache.objMainchara     = 1576;
    speartileCache.objSoundExists   = 1048;
    speartileCache.objSpeartilegen  = findObjectIndex(dw, "obj_speartilegen");
    speartileCache.objTime          = findObjectIndex(dw, "obj_time");
    speartileCache.ready = (speartileCache.conCount > 0 && speartileCache.facerCount > 0 &&
                            speartileCache.activeCount > 0);
    if (speartileCache.conCount > 1)
        fprintf(stderr, "NativeScripts: speartile 'con' has %d VARI entries — per-instance resolve enabled\n",
                speartileCache.conCount);
    if (speartileCache.conCount   == SPEARTILE_MAX_VARIDS ||
        speartileCache.facerCount == SPEARTILE_MAX_VARIDS ||
        speartileCache.activeCount== SPEARTILE_MAX_VARIDS) {
        fprintf(stderr, "NativeScripts: WARNING - speartile VARI buffer saturated at %d. "
                        "Increase SPEARTILE_MAX_VARIDS or speartile state-machine may break.\n",
                SPEARTILE_MAX_VARIDS);
    }
}






static bool speartile_collisionPoint(Runner* runner, DataWin* dw, Instance* self, float px, float py, int32_t targetObj) {
    if (targetObj < 0 || targetObj >= runner->instancesByObjMax || runner->instancesByObjInclParent == NULL) return false;
    Instance** list = runner->instancesByObjInclParent[targetObj];
    int32_t n = (int32_t)arrlen(list);

    for (int32_t i = 0; i < n; i++) {
        Instance* it = list[i];
        if (!it->active || it == self) continue;
        InstanceBBox bb = Collision_computeBBox(dw, it);
        if (!bb.valid) continue;
        if (bb.left > px || px >= bb.right || bb.top > py || py >= bb.bottom) continue;
        
        Sprite* spr = Collision_getSprite(dw, it);
        if (Collision_hasFrameMasks(spr)) {
            if (!Collision_pointInInstance(spr, it, px, py)) continue;
        }
        return true;
    }
    return false;
}

static void native_speartile_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!speartileCache.ready) return;
    DataWin* dw = ctx->dataWin;

    
    int32_t conId     = resolveSelfVarIdForInst(inst, speartileCache.conCandidates,     speartileCache.conCount);
    int32_t facerId   = resolveSelfVarIdForInst(inst, speartileCache.facerCandidates,   speartileCache.facerCount);
    int32_t soundedId = resolveSelfVarIdForInst(inst, speartileCache.soundedCandidates, speartileCache.soundedCount);
    int32_t activeId  = resolveSelfVarIdForInst(inst, speartileCache.activeCandidates,  speartileCache.activeCount);
    int32_t dutyId    = resolveSelfVarIdForInst(inst, speartileCache.dutyCandidates,    speartileCache.dutyCount);
    if (conId < 0 || facerId < 0 || activeId < 0) return;

    GMLReal con = selfReal(inst, conId);

    
    if (con == 0.0) {
        int32_t facer = selfInt(inst, facerId);
        if (facer == 1) {
            
            bool has1575 = false;
            int32_t n = (int32_t)arrlen(runner->instances);
            for (int32_t i = 0; i < n; i++) {
                Instance* it = runner->instances[i];
                if (it->active && it->objectIndex == speartileCache.objObstacle1575) { has1575 = true; break; }
            }
            if (has1575) {
                Instance* ot = (speartileCache.objTime >= 0) ? findInstanceByObject(runner, speartileCache.objTime) : NULL;
                int32_t leftK  = (ot && speartileCache.left_v  >= 0) ? (int32_t)RValue_toReal(Instance_getSelfVar(ot, speartileCache.left_v))  : 0;
                int32_t rightK = (ot && speartileCache.right_v >= 0) ? (int32_t)RValue_toReal(Instance_getSelfVar(ot, speartileCache.right_v)) : 0;
                int32_t upK    = (ot && speartileCache.up_v    >= 0) ? (int32_t)RValue_toReal(Instance_getSelfVar(ot, speartileCache.up_v))    : 0;
                int32_t downK  = (ot && speartileCache.down_v  >= 0) ? (int32_t)RValue_toReal(Instance_getSelfVar(ot, speartileCache.down_v))  : 0;

                if (leftK) {
                    inst->x -= 60.0f;
                    for (int32_t r = 0; r < 3; r++)
                        if (speartile_collisionPoint(runner, dw, inst, inst->x + 9.0f, inst->y + 35.0f, speartileCache.objSpeartileWall))
                            inst->x += 20.0f;
                }
                if (rightK) {
                    inst->x += 60.0f;
                    for (int32_t r = 0; r < 3; r++)
                        if (speartile_collisionPoint(runner, dw, inst, inst->x + 9.0f, inst->y + 35.0f, speartileCache.objSpeartileWall))
                            inst->x -= 20.0f;
                }
                if (downK) {
                    inst->y += 60.0f;
                    for (int32_t r = 0; r < 3; r++)
                        if (speartile_collisionPoint(runner, dw, inst, inst->x + 9.0f, inst->y + 35.0f, speartileCache.objSpeartileWall))
                            inst->y -= 20.0f;
                }
                if (upK) {
                    inst->y -= 60.0f;
                    for (int32_t r = 0; r < 3; r++)
                        if (speartile_collisionPoint(runner, dw, inst, inst->x + 9.0f, inst->y + 35.0f, speartileCache.objSpeartileWall))
                            inst->y += 20.0f;
                }
                Instance_setSelfVar(inst, facerId, RValue_makeReal(0.0));
            }
        }

        if (speartile_collisionPoint(runner, dw, inst, inst->x + 9.0f, inst->y + 35.0f, speartileCache.objSpeartileWall)) {
            Runner_destroyInstance(runner, inst);
            return;
        }

        inst->imageAlpha += 0.07f;
        if (inst->imageAlpha > 0.9f) {
            inst->imageAlpha = 1.0f;
            con = 1.0;
            inst->alarm[4] = 10;
            Instance_setSelfVar(inst, conId, RValue_makeReal(1.0));
        }
    }

    
    if (con == 2.0) {
        Instance* bud = Runner_createInstance(runner, inst->x, inst->y, 1365);
        if (bud) {
            bud->y += 9.0f;  
            bud->y -= 9.0f;  
            bud->spriteIndex = inst->spriteIndex;
            bud->imageSpeed  = 0.5f;
            bud->visible     = true;
            
            int32_t sprH = 0;
            if (bud->spriteIndex >= 0 && (uint32_t)bud->spriteIndex < dw->sprt.count)
                sprH = (int32_t)dw->sprt.sprites[bud->spriteIndex].height;
            bud->depth = 50000 - ((int32_t)(bud->y * 10.0f) + sprH * 10);
            if (speartileCache.spearbud >= 0)
                Instance_setSelfVar(inst, speartileCache.spearbud, RValue_makeReal((GMLReal)bud->instanceId));
        }
        con = 2.5;
        Instance_setSelfVar(inst, conId, RValue_makeReal(2.5));
    }

    
    if (fabs(con - 2.5) < 1e-6) {
        GMLReal budId = (speartileCache.spearbud >= 0) ? selfReal(inst, speartileCache.spearbud) : -4.0;
        Instance* bud = NULL;
        int32_t n = (int32_t)arrlen(runner->instances);
        for (int32_t i = 0; i < n; i++) {
            Instance* it = runner->instances[i];
            if (it->active && (int32_t)it->instanceId == (int32_t)budId) { bud = it; break; }
        }

        int32_t active = selfInt(inst, activeId);
        if (bud && bud->imageIndex >= 1.5f && active == 1) {
            
            InstanceBBox sb = Collision_computeBBox(dw, inst);
            if (sb.valid) {
                for (int32_t i = 0; i < n; i++) {
                    Instance* mc = runner->instances[i];
                    if (!mc->active || mc == inst) continue;
                    if (!Collision_matchesTarget(dw, mc, speartileCache.objMainchara)) continue;
                    InstanceBBox mb = Collision_computeBBox(dw, mc);
                    if (!mb.valid) continue;
                    if (!(sb.left >= mb.right || mb.left >= sb.right ||
                          sb.top >= mb.bottom || mb.top >= sb.bottom)) {
                        Runner_executeEvent(runner, inst, 7, 13); 
                        break;
                    }
                }
            }
            
            int32_t sounded = (soundedId >= 0) ? selfInt(inst, soundedId) : 1;
            if (sounded == 0) {
                bool has1048 = false;
                for (int32_t i = 0; i < n; i++) {
                    Instance* it = runner->instances[i];
                    if (it->active && it->objectIndex == speartileCache.objSoundExists) { has1048 = true; break; }
                }
                if (has1048 && speartileCache.objSpeartilegen >= 0) {
                    Instance* gen = findInstanceByObject(runner, speartileCache.objSpeartilegen);
                    if (gen && speartileCache.sound2 >= 0)
                        Instance_setSelfVar(gen, speartileCache.sound2, RValue_makeReal(1.0));
                    if (soundedId >= 0)
                        Instance_setSelfVar(inst, soundedId, RValue_makeReal(1.0));
                }
            }
        }
        if (bud && bud->imageIndex >= 3.0f) {
            bud->imageSpeed = 0.0f;
            con = 3.0;
            inst->alarm[4] = 5;
            Instance_setSelfVar(inst, conId, RValue_makeReal(3.0));
        }
    }

    
    if (con == 4.0) {
        GMLReal budId = (speartileCache.spearbud >= 0) ? selfReal(inst, speartileCache.spearbud) : -4.0;
        Instance* bud = NULL;
        int32_t n = (int32_t)arrlen(runner->instances);
        for (int32_t i = 0; i < n; i++) {
            Instance* it = runner->instances[i];
            if (it->active && (int32_t)it->instanceId == (int32_t)budId) { bud = it; break; }
        }
        inst->imageAlpha -= 0.1f;
        if (bud) bud->imageAlpha = inst->imageAlpha;
        if (inst->imageAlpha < 0.1f) {
            if (bud) Runner_destroyInstance(runner, bud);
            Runner_destroyInstance(runner, inst);
            return;
        }
    }

    
    int32_t duty = (dutyId >= 0) ? selfInt(inst, dutyId) : 0;
    if (duty == 1) {
        Instance* mc = (speartileCache.objMainchara >= 0) ? findInstanceByObject(runner, speartileCache.objMainchara) : NULL;
        if (mc) {
            inst->x = mc->x;
            inst->y = mc->y;
        }
        if (dutyId >= 0)
            Instance_setSelfVar(inst, dutyId, RValue_makeReal(0.0));
        
        inst->x = (float)(floor((inst->x / 20.0f) + 0.5) * 20.0);
        inst->y = (float)(floor((inst->y / 20.0f) + 0.5) * 20.0);
    }

    
    int32_t active = selfInt(inst, activeId);
    if (active == 2) {
        if (con >= 2.5) {
            GMLReal budId = (speartileCache.spearbud >= 0) ? selfReal(inst, speartileCache.spearbud) : -4.0;
            int32_t n = (int32_t)arrlen(runner->instances);
            for (int32_t i = 0; i < n; i++) {
                Instance* it = runner->instances[i];
                if (it->active && (int32_t)it->instanceId == (int32_t)budId) {
                    Runner_destroyInstance(runner, it);
                    break;
                }
            }
        }
        Runner_destroyInstance(runner, inst);
    }
}







static struct {
    int32_t part, side, angel;
    int32_t gIdealborder;
    int32_t objHeart;
    bool ready;
} dummybulletStepCache = { .ready = false };

static void initDummybulletStepCache(VMContext* ctx, DataWin* dw) {
    dummybulletStepCache.part   = findSelfVarId(dw, "part");
    dummybulletStepCache.side   = findSelfVarId(dw, "side");
    dummybulletStepCache.angel  = findSelfVarId(dw, "angel");
    dummybulletStepCache.gIdealborder = findGlobalVarId(ctx, "idealborder");
    dummybulletStepCache.objHeart     = findObjectIndex(dw, "obj_heart");
    dummybulletStepCache.ready = (dummybulletStepCache.part >= 0 && dummybulletStepCache.side >= 0);
}

static void native_dummybullet_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!dummybulletStepCache.ready) return;

    GMLReal part = selfReal(inst, dummybulletStepCache.part);
    if (part >= 1.0) return;

    
    
    
    
    
    
    float heartX = inst->x, heartY = inst->y;
    if (dummybulletStepCache.objHeart >= 0) {
        Instance* h = findInstanceByObject(runner, dummybulletStepCache.objHeart);
        if (h) { heartX = h->x; heartY = h->y; }
    }
    double dx = (heartX + 2.0) - inst->x;
    double dy = (heartY + 2.0) - inst->y;
    double pd = atan2(-dy, dx) * (180.0 / M_PI);
    if (pd < 0.0) pd += 360.0;
    if (dummybulletStepCache.angel >= 0)
        Instance_setSelfVar(inst, dummybulletStepCache.angel, RValue_makeReal(pd));

    inst->speed = 3.0f;
    
    float radDir = inst->direction * (float)(M_PI / 180.0);
    inst->hspeed =  3.0f * cosf(radDir);
    inst->vspeed = -3.0f * sinf(radDir);

    
    int32_t side = selfInt(inst, dummybulletStepCache.side);
    if (dummybulletStepCache.gIdealborder >= 0) {
        int64_t k = ((int64_t)dummybulletStepCache.gIdealborder << 32);
        ptrdiff_t i0 = hmgeti(ctx->globalArrayMap, k | 0u);
        ptrdiff_t i1 = hmgeti(ctx->globalArrayMap, k | 1u);
        ptrdiff_t i2 = hmgeti(ctx->globalArrayMap, k | 2u);
        ptrdiff_t i3 = hmgeti(ctx->globalArrayMap, k | 3u);
        float b0 = (i0 >= 0) ? (float)RValue_toReal(ctx->globalArrayMap[i0].value) : 0.0f;
        float b1 = (i1 >= 0) ? (float)RValue_toReal(ctx->globalArrayMap[i1].value) : 0.0f;
        float b2 = (i2 >= 0) ? (float)RValue_toReal(ctx->globalArrayMap[i2].value) : 0.0f;
        float b3 = (i3 >= 0) ? (float)RValue_toReal(ctx->globalArrayMap[i3].value) : 0.0f;

        if ((side == 0 && inst->x > b0 + 4.0f) ||
            (side == 1 && inst->x < b1 - 22.0f) ||
            (side == 2 && inst->y > b2 + 4.0f) ||
            (side == 3 && inst->y < b3 - 22.0f)) {
            Runner_executeEvent(runner, inst, 7, 11); 
        }
    }
}






static struct {
    int32_t objMaddumDrawer;
    bool ready;
} dummyshotCache = { .ready = false };

static void initDummyshotCache(DataWin* dw) {
    dummyshotCache.objMaddumDrawer = findObjectIndex(dw, "obj_maddum_drawer");
    dummyshotCache.ready = true;
}

static void native_dummyshot_Collision288(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!dummyshotCache.ready) return;

    
    int32_t n = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < n; i++) {
        Instance* it = runner->instances[i];
        if (!it->active || it->objectIndex != 288) continue;
        Runner_executeEvent(runner, it, 7, 13); 
        Runner_executeEvent(runner, it, 7, 17); 
    }

    
    if (dummyshotCache.objMaddumDrawer >= 0) {
        Instance* mdrw = findInstanceByObject(runner, dummyshotCache.objMaddumDrawer);
        if (mdrw && mdrw->alarm[5] < 2) {
            for (int32_t i = 0; i < n; i++) {
                Instance* it = runner->instances[i];
                if (it->active && it->objectIndex == 289)
                    Runner_executeEvent(runner, it, 7, 12); 
            }
        }
    }

    Runner_destroyInstance(runner, inst);
}







static struct {
    int32_t juice, destroy;
    int32_t objHeart;
    bool ready;
} dummymissleStepCache = { .ready = false };

static void initDummymissleStepCache(DataWin* dw) {
    dummymissleStepCache.juice   = findSelfVarId(dw, "juice");
    dummymissleStepCache.destroy = findSelfVarId(dw, "destroy");
    dummymissleStepCache.objHeart = findObjectIndex(dw, "obj_heart");
    dummymissleStepCache.ready = (dummymissleStepCache.juice >= 0 && dummymissleStepCache.destroy >= 0);
}

static void native_dummymissle_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!dummymissleStepCache.ready) return;

    if (inst->speed > 6.0f) inst->friction = 0.0f;

    GMLReal juice = selfReal(inst, dummymissleStepCache.juice);
    if (juice > 0.0) {
        inst->imageAngle = inst->direction;
        float curdir = inst->direction;
        float idealdir = curdir;
        if (dummymissleStepCache.objHeart >= 0) {
            Instance* h = findInstanceByObject(runner, dummymissleStepCache.objHeart);
            if (h) {
                double dx = (h->x + 10.0) - inst->x;
                double dy = (h->y + 10.0) - inst->y;
                double pd = atan2(-dy, dx) * (180.0 / M_PI);
                if (pd < 0.0) pd += 360.0;
                idealdir = (float)pd;
            }
        }

        float facingMinusTarget = curdir - idealdir;
        float angleDiff = facingMinusTarget;
        if (fabsf(facingMinusTarget) > 180.0f) {
            if (curdir > idealdir) angleDiff = -1.0f * ((360.0f - curdir) + idealdir);
            else                   angleDiff = (360.0f - idealdir) + curdir;
        }

        if (fabsf(angleDiff) > 4.0f) {
            float ad = fabsf(angleDiff);
            float dirspeed = 1.0f;
            if (ad >  10.0f) dirspeed =  2.0f;
            if (ad >  20.0f) dirspeed =  3.0f;
            if (ad >  30.0f) dirspeed =  4.0f;
            if (ad >  40.0f) dirspeed =  5.0f;
            if (ad >  50.0f) dirspeed =  6.0f;
            if (ad >  60.0f) dirspeed =  7.0f;
            if (ad >  70.0f) dirspeed =  8.0f;
            if (ad >  80.0f) dirspeed =  9.0f;
            if (ad >  90.0f) dirspeed = 10.0f;
            if (ad > 100.0f) dirspeed = 11.0f;
            if (angleDiff < 0.0f) dirspeed = -dirspeed;
            inst->direction -= dirspeed;
            
            float rad = inst->direction * (float)(M_PI / 180.0);
            inst->hspeed =  inst->speed * cosf(rad);
            inst->vspeed = -inst->speed * sinf(rad);
        }
    }

    juice -= 1.0;
    Instance_setSelfVar(inst, dummymissleStepCache.juice, RValue_makeReal(juice));

    if (juice < 60.0) inst->imageIndex = 1.0f;
    if (juice < 30.0) inst->imageIndex = 2.0f;
    if (juice < 0.0)  inst->imageIndex = 3.0f;

    if (juice < -60.0) {
        GMLReal destroy = selfReal(inst, dummymissleStepCache.destroy);
        if (destroy == 0.0) {
            Instance_setSelfVar(inst, dummymissleStepCache.destroy, RValue_makeReal(1.0));
        }
    }
}

















static struct {
    int32_t siner, timer;
    bool ready;
} confettiCache = { .ready = false };

static void initConfettiCache(DataWin* dw) {
    confettiCache.siner = findSelfVarId(dw, "siner");
    confettiCache.timer = findSelfVarId(dw, "timer");
    confettiCache.ready = (confettiCache.siner >= 0 && confettiCache.timer >= 0);
}

static void native_confetti_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!confettiCache.ready) return;

    
    ptrdiff_t si = hmgeti(inst->selfVars, confettiCache.siner);
    ptrdiff_t ti = hmgeti(inst->selfVars, confettiCache.timer);
    if (si < 0 || ti < 0) return;  

    
    
    
    RValue* sv = &inst->selfVars[si].value;
    RValue* tv = &inst->selfVars[ti].value;
    GMLReal siner = (sv->type == RVALUE_REAL) ? sv->real : RValue_toReal(*sv);
    GMLReal timer = (tv->type == RVALUE_REAL) ? tv->real : RValue_toReal(*tv);
    siner += 1.0;
    timer -= 1.0;

    
    inst->x += (float)(GMLReal_sin(siner / 3.0) * 2.0);

    if (timer < 20.0) inst->imageAlpha -= 0.05f;

    
    
    sv->real = siner; sv->type = RVALUE_REAL;
    tv->real = timer; tv->type = RVALUE_REAL;

    if (inst->imageAlpha <= 0.05f) {
        Runner_destroyInstance(runner, inst);
    }
}






static struct {
    int32_t siner;
    int32_t objFlyjar;  
    int32_t objFlyjarExists; 
    bool ready;
} jarflyCache = { .ready = false };

static void initJarflyCache(DataWin* dw) {
    jarflyCache.siner     = findSelfVarId(dw, "siner");
    jarflyCache.objFlyjar = findObjectIndex(dw, "obj_flyjar");
    jarflyCache.objFlyjarExists = 306;
    jarflyCache.ready = (jarflyCache.siner >= 0);
}

static void native_jarfly_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!jarflyCache.ready) return;
    DataWin* dw = ctx->dataWin;

    GMLReal siner = selfReal(inst, jarflyCache.siner);
    inst->x += (float)(sin(siner / 4.0) / 2.0);
    inst->y += (float)(cos(siner / 4.0) / 2.0);
    siner += 1.0;
    Instance_setSelfVar(inst, jarflyCache.siner, RValue_makeReal(siner));

    
    Instance* jar = NULL;
    int32_t n = (int32_t)arrlen(runner->instances);
    for (int32_t i = 0; i < n; i++) {
        Instance* it = runner->instances[i];
        if (it->active && it->objectIndex == jarflyCache.objFlyjarExists) { jar = it; break; }
    }
    if (jar == NULL) {
        Runner_destroyInstance(runner, inst);
        return;
    }

    
    InstanceBBox jb = Collision_computeBBox(dw, jar);
    if (!jb.valid) return;

    inst->imageAlpha = jar->imageAlpha;

    if (inst->x > jb.right) {
        inst->x -= 4.0f;
        if (inst->hspeed > 0.0f) inst->hspeed = -inst->hspeed;
    }
    if (inst->x < jb.left) {
        inst->x += 4.0f;
        if (inst->hspeed < 0.0f) inst->hspeed = -inst->hspeed;
    }
    if (inst->y < jb.top) {
        inst->y += 4.0f;
        if (inst->vspeed < 0.0f) inst->vspeed = -inst->vspeed;
    }
    if (inst->y > jb.bottom) {
        inst->y -= 4.0f;
        if (inst->vspeed > 0.0f) inst->vspeed = -inst->vspeed;
    }
    
    Instance_computeSpeedFromComponents(inst);
}







static struct {
    int32_t mettamt;
    int32_t gLanguage;
    int32_t objQuestionasker;
    bool ready;
} mettatonnnWriterCache = { .ready = false };

static void initMettatonnnWriterCache(VMContext* ctx, DataWin* dw) {
    mettatonnnWriterCache.mettamt    = findSelfVarId(dw, "mettamt");
    mettatonnnWriterCache.gLanguage  = findGlobalVarId(ctx, "language");
    mettatonnnWriterCache.objQuestionasker = findObjectIndex(dw, "obj_questionasker");
    mettatonnnWriterCache.ready = true;
}

static void native_mettatonnnWriter_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)inst;
    if (!mettatonnnWriterCache.ready || runner->renderer == NULL || runner->currentRoom == NULL) return;
    Renderer* r = runner->renderer;

    
    const char* lang = (mettatonnnWriterCache.gLanguage >= 0) ? globalString(ctx, mettatonnnWriterCache.gLanguage) : NULL;
    bool isJa = (lang && strcmp(lang, "ja") == 0);

    float rw = (float)runner->currentRoom->width;
    float rh = (float)runner->currentRoom->height;

    
    const char* ch = isJa ? "ン" : "n";
    float spacing = isJa ? 28.0f : 14.0f;
    float xstart_top, ystart_top;
    int32_t limit_top;
    float xstart_right, ystart_right;
    int32_t limit_right;
    float xstart_bottom, ystart_bottom;
    int32_t limit_bottom;
    float xstart_left, ystart_left;

    if (isJa) {
        xstart_top = 317.0f;  ystart_top = 97.0f;
        limit_top = (int32_t)floor((rw - 6.0f - xstart_top) / spacing);
        xstart_right = rw - 6.0f;
        ystart_right = ystart_top + 30.0f;
        limit_right = (int32_t)floor((rh - 16.0f - ystart_right) / spacing);
        xstart_bottom = rw - 34.0f;
        ystart_bottom = rh - 16.0f;
        limit_bottom = (int32_t)floor((6.0f - xstart_bottom) / -spacing);
        xstart_left = 6.0f;
        ystart_left = rh - 46.0f;
    } else {
        xstart_top = 431.0f;      ystart_top = 82.0f;          limit_top = 13;
        xstart_right = rw - 6.0f; ystart_right = 116.0f;       limit_right = 20;
        xstart_bottom = rw - 40.0f; ystart_bottom = rh - 20.0f; limit_bottom = 32;
        xstart_left = 0.0f;        ystart_left = 0.0f;
    }

    
    int32_t count = 0;
    if (mettatonnnWriterCache.objQuestionasker >= 0 && mettatonnnWriterCache.mettamt >= 0) {
        Instance* qa = findInstanceByObject(runner, mettatonnnWriterCache.objQuestionasker);
        if (qa) count = (int32_t)RValue_toReal(Instance_getSelfVar(qa, mettatonnnWriterCache.mettamt));
    }

    
    int32_t count_top = (count > limit_top) ? limit_top : count;
    count -= count_top;
    int32_t count_right = (count > limit_right) ? limit_right : count;
    count -= count_right;
    int32_t count_bottom = (count > limit_bottom) ? limit_bottom : count;
    count -= count_bottom;
    int32_t count_left = count;

    
    nativeSetFont(r, ctx, 1);
    r->drawColor = 0xFFFFFFu;

    
    char* chProc = TextUtils_preprocessGmlTextIfNeeded(runner, ch);

    float randScale = 1.0f / (float)RAND_MAX;
    #define RND01() ((float)rand() * randScale >= 0.5f ? 1.0f : 0.0f)

    
    float xx = xstart_top, yy = ystart_top;
    for (int32_t i = 0; i < count_top; i++) {
        r->vtable->drawText(r, chProc, xx + RND01(), yy + RND01(), 1.0f, 1.0f, 0.0f);
        xx += spacing;
    }

    
    if (!isJa) spacing += 2.0f;

    
    xx = xstart_right; yy = ystart_right;
    for (int32_t i = 0; i < count_right; i++) {
        r->vtable->drawText(r, chProc, xx + RND01(), yy + RND01(), 1.0f, 1.0f, 270.0f);
        yy += spacing;
    }

    
    xx = xstart_bottom; yy = ystart_bottom;
    for (int32_t i = 0; i < count_bottom; i++) {
        r->vtable->drawText(r, chProc, xx + RND01(), yy + RND01(), 1.0f, 1.0f, 180.0f);
        xx -= spacing;
    }

    
    xx = xstart_left; yy = ystart_left;
    for (int32_t i = 0; i < count_left; i++) {
        r->vtable->drawText(r, chProc, xx + RND01(), yy + RND01(), 1.0f, 1.0f, 90.0f);
        yy -= spacing;
    }

    #undef RND01
    free(chProc);
}






static struct {
    int32_t size;
    bool ready;
} bouncersteamCreateCache = { .ready = false };

static void initBouncersteamCreateCache(DataWin* dw) {
    bouncersteamCreateCache.size = findSelfVarId(dw, "size");
    bouncersteamCreateCache.ready = (bouncersteamCreateCache.size >= 0);
}

static void native_bouncersteam_Create0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    if (!bouncersteamCreateCache.ready) return;

    inst->x += 10.0f;
    inst->y += 10.0f;
    
    float randScale = 1.0f / (float)RAND_MAX;
    inst->imageAngle = (float)rand() * randScale * 360.0f;
    inst->imageXscale = 0.4f;
    inst->imageYscale = 0.4f;
    Instance_setSelfVar(inst, bouncersteamCreateCache.size, RValue_makeReal(0.4));
    inst->alarm[0] = 0;
    inst->direction = 80.0f + (float)rand() * randScale * 20.0f;
    inst->speed = 3.0f;
    inst->friction = 0.1f;
    Instance_computeComponentsFromSpeed(inst);
}





static void native_bounceright_Alarm1(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!bouncerightCache.ready) { inst->alarm[1] = 10; return; }
    int32_t con = selfInt(inst, bouncerightCache.con);
    if (con == 0) Runner_createInstance(runner, inst->x, inst->y, 1534);
    inst->alarm[1] = 10;
}






static struct {
    int32_t timer;
    int32_t objKillervisage;
    bool ready;
} chimesparkleCache = { .ready = false };

static void initChimesparkleCache(DataWin* dw) {
    chimesparkleCache.timer = findSelfVarId(dw, "timer");
    chimesparkleCache.objKillervisage = findObjectIndex(dw, "obj_killervisage");
    chimesparkleCache.ready = (chimesparkleCache.timer >= 0);
}

static void native_chimesparkle_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!chimesparkleCache.ready) return;

    ptrdiff_t ti = hmgeti(inst->selfVars, chimesparkleCache.timer);
    if (ti < 0) return;
    RValue* tv = &inst->selfVars[ti].value;
    GMLReal timer = (tv->type == RVALUE_REAL) ? tv->real : RValue_toReal(*tv);

    if (timer < 30.0) inst->imageAlpha += 0.1f;
    timer += 1.0;
    if (timer > 20.0) {
        inst->imageAlpha -= 0.04f;
        if (inst->imageAlpha < 0.05f) {
            Runner_destroyInstance(runner, inst);
            return;
        }
    }
    tv->real = timer; tv->type = RVALUE_REAL;

    
    if (chimesparkleCache.objKillervisage >= 0) {
        Instance* kv = findInstanceByObject(runner, chimesparkleCache.objKillervisage);
        if (kv && inst->imageAlpha > kv->imageAlpha) inst->imageAlpha = kv->imageAlpha;
    }
}





static struct {
    int32_t size, ang;
    bool ready;
} sugarbulletCache = { .ready = false };

static void initSugarbulletCache(DataWin* dw) {
    sugarbulletCache.size = findSelfVarId(dw, "size");
    sugarbulletCache.ang  = findSelfVarId(dw, "ang");
    sugarbulletCache.ready = (sugarbulletCache.size >= 0 && sugarbulletCache.ang >= 0);
}

static void native_sugarbullet_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!sugarbulletCache.ready || runner->currentRoom == NULL) return;

    if (inst->vspeed > 0.0f) inst->depth = 2;

    ptrdiff_t si = hmgeti(inst->selfVars, sugarbulletCache.size);
    ptrdiff_t ai = hmgeti(inst->selfVars, sugarbulletCache.ang);
    if (si < 0 || ai < 0) return;
    RValue* sv = &inst->selfVars[si].value;
    RValue* av = &inst->selfVars[ai].value;
    GMLReal size = (sv->type == RVALUE_REAL) ? sv->real : RValue_toReal(*sv);
    GMLReal ang  = (av->type == RVALUE_REAL) ? av->real : RValue_toReal(*av);

    if (size < 1.0) size += 0.04;
    inst->imageXscale = (float)size;
    inst->imageYscale = (float)size;
    inst->imageAngle += (float)ang;

    sv->real = size; sv->type = RVALUE_REAL;

    
    int32_t viewIdx = runner->viewCurrent;
    float viewX = (float)runner->currentRoom->views[viewIdx].viewX;
    float viewY = (float)runner->currentRoom->views[viewIdx].viewY;
    if (inst->y > viewY + 250.0f ||
        inst->x < viewX -  10.0f ||
        inst->x > viewX + 320.0f) {
        Runner_destroyInstance(runner, inst);
    }
}






#define SUGARSHOT_MAX_ELIGIBLE_VARIDS 16
static struct {
    int32_t eligibleCandidates[SUGARSHOT_MAX_ELIGIBLE_VARIDS];
    int32_t eligibleCount;
    bool ready;
} sugarshotCollisionCache = { .ready = false };

static void initSugarshotCollisionCache(DataWin* dw) {
    sugarshotCollisionCache.eligibleCount = findAllSelfVarIds(dw, "eligible",
                                                              sugarshotCollisionCache.eligibleCandidates,
                                                              SUGARSHOT_MAX_ELIGIBLE_VARIDS);
    sugarshotCollisionCache.ready = (sugarshotCollisionCache.eligibleCount > 0);
}


static inline void sugarshot_collisionImpl(VMContext* ctx, Runner* runner, Instance* inst,
                                           int32_t selfUserEvent) {
    
    Runner_executeEvent(runner, inst, 7, 10 + selfUserEvent);

    
    Instance* other = (Instance*)ctx->otherInstance;
    if (other && other->active && sugarshotCollisionCache.ready) {
        int32_t eligibleId = resolveSelfVarIdForInst(other,
                                                     sugarshotCollisionCache.eligibleCandidates,
                                                     sugarshotCollisionCache.eligibleCount);
        if (eligibleId >= 0) {
            int32_t eligible = (int32_t)RValue_toReal(Instance_getSelfVar(other, eligibleId));
            if (eligible == 1) {
                Runner_executeEvent(runner, other, 7, 10); 
            }
        }
    }

    
    BuiltinFunc snd = VMBuiltins_find("snd_play");
    if (snd) { RValue a = RValue_makeReal(107.0); RValue r = snd(ctx, &a, 1); RValue_free(&r); }
}




static void native_sugarbullet_Collision1187(VMContext* ctx, Runner* runner, Instance* inst) {
    sugarshot_collisionImpl(ctx, runner, inst, 1); 
}




static void native_milkofhell_shot_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx; (void)runner;
    inst->imageXscale += 0.1f;
    inst->imageYscale += 0.05f;
}






static void native_milkofhell_shot_Other10(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    Instance* splat = Runner_createInstance(runner, inst->x, inst->y, 1178);
    if (splat) {
        splat->imageAngle  = inst->imageAngle;
        splat->imageXscale = inst->imageXscale;
        splat->imageYscale = inst->imageYscale;
        splat->speed       = inst->speed;
        splat->direction   = inst->direction;
        splat->friction    = 2.0f;
        Instance_computeComponentsFromSpeed(splat);
    }
    for (int32_t i = 0; i < 8; i++) {
        Runner_createInstance(runner, inst->x, inst->y, 1179);
    }
    Runner_destroyInstance(runner, inst);
}




static void native_milkofhell_shot_Collision1187(VMContext* ctx, Runner* runner, Instance* inst) {
    sugarshot_collisionImpl(ctx, runner, inst, 0); 
}





static struct {
    int32_t ang;
    bool ready;
} mettEggbulletCache = { .ready = false };

static void initMettEggbulletCache(DataWin* dw) {
    mettEggbulletCache.ang = findSelfVarId(dw, "ang");
    mettEggbulletCache.ready = (mettEggbulletCache.ang >= 0);
}

static void native_mettEggbullet_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!mettEggbulletCache.ready || runner->currentRoom == NULL) return;

    if (inst->imageAlpha < 1.0f) inst->imageAlpha += 0.2f;

    ptrdiff_t ai = hmgeti(inst->selfVars, mettEggbulletCache.ang);
    if (ai >= 0) {
        RValue* av = &inst->selfVars[ai].value;
        GMLReal ang = (av->type == RVALUE_REAL) ? av->real : RValue_toReal(*av);
        inst->imageAngle += (float)ang;
    }

    int32_t viewIdx = runner->viewCurrent;
    float viewX = (float)runner->currentRoom->views[viewIdx].viewX;
    float viewY = (float)runner->currentRoom->views[viewIdx].viewY;
    if (inst->y > viewY + 245.0f ||
        inst->x < viewX -   4.0f ||
        inst->x > viewX + 324.0f) {
        Runner_destroyInstance(runner, inst);
    }
}






static struct {
    int32_t t_v, aa_v;
    bool ready;
} steamplume2Cache = { .ready = false };

static void initSteamplume2Cache(DataWin* dw) {
    steamplume2Cache.t_v  = findSelfVarId(dw, "t");
    steamplume2Cache.aa_v = findSelfVarId(dw, "aa");
    steamplume2Cache.ready = (steamplume2Cache.t_v >= 0 && steamplume2Cache.aa_v >= 0);
}

static void native_steamplume2_Step0(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!steamplume2Cache.ready) return;

    inst->imageXscale += 0.1f;
    inst->imageYscale += 0.1f;

    
    ptrdiff_t ti = hmgeti(inst->selfVars, steamplume2Cache.t_v);
    ptrdiff_t ai = hmgeti(inst->selfVars, steamplume2Cache.aa_v);
    if (ti < 0 || ai < 0) return;
    RValue* tv = &inst->selfVars[ti].value;
    RValue* av = &inst->selfVars[ai].value;
    GMLReal t  = (tv->type == RVALUE_REAL) ? tv->real : RValue_toReal(*tv);
    GMLReal aa = (av->type == RVALUE_REAL) ? av->real : RValue_toReal(*av);

    t += 1.0;
    if (t > 7.0) inst->imageAlpha -= 0.08f;

    tv->real = t; tv->type = RVALUE_REAL;

    if (inst->imageAlpha <= 0.02f) {
        Runner_destroyInstance(runner, inst);
        return;
    }

    inst->imageAngle += (float)aa;
}










#define METTNEWS_PART_MAX_VARIDS 128
static struct {
    int32_t onCandidates[METTNEWS_PART_MAX_VARIDS]; int32_t onCount;
    int32_t stayx, stayy, ang;
    int32_t objMainchara;
    int32_t obj185;
    bool ready;
} mettnewsPartCache = { .ready = false };

static void initMettnewsPartCache(DataWin* dw) {
    mettnewsPartCache.onCount = findAllSelfVarIds(dw, "on",
                                                  mettnewsPartCache.onCandidates,
                                                  METTNEWS_PART_MAX_VARIDS);
    mettnewsPartCache.stayx = findSelfVarId(dw, "stayx");
    mettnewsPartCache.stayy = findSelfVarId(dw, "stayy");
    mettnewsPartCache.ang   = findSelfVarId(dw, "ang");
    mettnewsPartCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    mettnewsPartCache.obj185 = 185;
    mettnewsPartCache.ready = (mettnewsPartCache.onCount > 0 &&
                               mettnewsPartCache.stayx >= 0 && mettnewsPartCache.stayy >= 0);
}

static void native_mettnewsPart_Step2(VMContext* ctx, Runner* runner, Instance* inst) {
    (void)ctx;
    if (!mettnewsPartCache.ready || runner->currentRoom == NULL) return;

    int32_t onId = resolveSelfVarIdForInst(inst, mettnewsPartCache.onCandidates,
                                                  mettnewsPartCache.onCount);
    if (onId < 0) return;
    int32_t on = (int32_t)RValue_toReal(Instance_getSelfVar(inst, onId));

    int32_t viewIdx = runner->viewCurrent;
    RoomView* view = &runner->currentRoom->views[viewIdx];

    
    if (on == 1) {
        bool has185 = false;
        int32_t n = (int32_t)arrlen(runner->instances);
        for (int32_t i = 0; i < n; i++) {
            Instance* it = runner->instances[i];
            if (it->active && it->objectIndex == mettnewsPartCache.obj185) { has185 = true; break; }
        }
        if (!has185) {
            Instance* mc = (mettnewsPartCache.objMainchara >= 0)
                         ? findInstanceByObject(runner, mettnewsPartCache.objMainchara) : NULL;
            if (mc) {
                view->viewX = (int32_t)(floor((double)mc->x - (double)view->viewWidth  / 2.0 + 10.0 + 0.5));
                view->viewY = (int32_t)(floor((double)mc->y - (double)view->viewHeight / 2.0 + 10.0 + 0.5));
                if (view->viewY <= 0) view->viewY = 0;
            }
        }

        
        GMLReal stayx = selfReal(inst, mettnewsPartCache.stayx);
        GMLReal stayy = selfReal(inst, mettnewsPartCache.stayy);
        inst->x = (float)view->viewX + (float)stayx;
        inst->y = (float)view->viewY + (float)stayy;
        if (view->viewY <= 0) view->viewY = 0;
    }

    if (on == 0) {
        inst->gravity = 0.4f;
        if (mettnewsPartCache.ang >= 0) {
            GMLReal ang = selfReal(inst, mettnewsPartCache.ang);
            inst->imageAngle += (float)ang;
        }
        if (inst->y > (float)runner->currentRoom->height) {
            Runner_destroyInstance(runner, inst);
            return;
        }
    }

    
    if (view->viewY <= 0) view->viewY = 0;
}

#define SNOWFLOOR_MOVING_VARIDS 12
static struct {
    int32_t dodraw, snowx, snowy, moveme;
    int32_t movingCandidates[SNOWFLOOR_MOVING_VARIDS];
    int32_t movingCount;
    int32_t gFlag;
    int32_t objMainchara;
    bool ready;
} snowfloorCache = { .ready = false };

static void initSnowfloorCache(VMContext* ctx, DataWin* dw) {
    snowfloorCache.dodraw = findSelfVarId(dw, "dodraw");
    snowfloorCache.snowx  = findSelfVarId(dw, "snowx");
    snowfloorCache.snowy  = findSelfVarId(dw, "snowy");
    snowfloorCache.moveme = findSelfVarId(dw, "moveme");
    
    
    snowfloorCache.movingCount = findAllSelfVarIds(dw, "moving",
                                                   snowfloorCache.movingCandidates,
                                                   SNOWFLOOR_MOVING_VARIDS);
    snowfloorCache.gFlag = findGlobalVarId(ctx, "flag");
    snowfloorCache.objMainchara = findObjectIndex(dw, "obj_mainchara");
    snowfloorCache.ready = (snowfloorCache.dodraw >= 0 && snowfloorCache.snowx >= 0 &&
                            snowfloorCache.snowy >= 0 && snowfloorCache.moveme >= 0);
}

static uint32_t g_snow_rand_seed = 12345;
static inline float fast_rand_float(void) {
    g_snow_rand_seed ^= g_snow_rand_seed << 13;
    g_snow_rand_seed ^= g_snow_rand_seed >> 17;
    g_snow_rand_seed ^= g_snow_rand_seed << 5;
    return (float)(g_snow_rand_seed & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

typedef struct {
    RValue* sxv; RValue* syv; RValue* mmv; RValue* ddv;
} SnowFlakeCacheStruct;

static void native_snowfloor_Draw0(VMContext* ctx, Runner* runner, Instance* inst) {
    if (!snowfloorCache.ready || runner->renderer == NULL) return;
    Renderer* r = runner->renderer;

    Instance* mc = (snowfloorCache.objMainchara >= 0)
                 ? findInstanceByObject(runner, snowfloorCache.objMainchara) : NULL;

    bool hasMainchara = (mc != NULL);
    bool maincharaFar = false;
    InstanceBBox mcBB = { 0, 0, 0, 0, false };
    int32_t mcMoving = 0;

    if (hasMainchara) {
        mcBB = Collision_computeBBox(ctx->dataWin, mc);

        float dx = mc->x - inst->x;
        float dy = mc->y - inst->y;
        if (dx < -60.0f || dx > 60.0f || dy < -60.0f || dy > 60.0f) {
            maincharaFar = true;
        }

        int32_t movingId = resolveSelfVarIdForInst(mc,
                                                   snowfloorCache.movingCandidates,
                                                   snowfloorCache.movingCount);
        if (movingId >= 0)
            mcMoving = (int32_t)RValue_toReal(Instance_getSelfVar(mc, movingId));
    }

    bool roomIs57 = (runner->currentRoomIndex == 57);
    bool needFlag64Set = false;
    if (roomIs57 && snowfloorCache.gFlag >= 0) {
        int64_t flagKey = ((int64_t)snowfloorCache.gFlag << 32) | 64u;
        ptrdiff_t fi = hmgeti(ctx->globalArrayMap, flagKey);
        GMLReal f64 = (fi >= 0) ? RValue_toReal(ctx->globalArrayMap[fi].value) : 0.0;
        needFlag64Set = (f64 == 0.0);
    }

    // Находим границы экрана (Culling), чтобы не рисовать снег, который мы не видим
    int viewIdx = runner->viewCurrent;
    float viewX = runner->currentRoom ? (float)runner->currentRoom->views[viewIdx].viewX : 0.0f;
    float viewY = runner->currentRoom ? (float)runner->currentRoom->views[viewIdx].viewY : 0.0f;
    float viewW = runner->currentRoom ? (float)runner->currentRoom->views[viewIdx].viewWidth : 320.0f;
    float viewH = runner->currentRoom ? (float)runner->currentRoom->views[viewIdx].viewHeight : 240.0f;

    // Расширяем границы на 10 пикселей
    float vL = viewX - 10.0f, vR = viewX + viewW + 10.0f;
    float vT = viewY - 10.0f, vB = viewY + viewH + 10.0f;

    bool canCollide = (hasMainchara && !maincharaFar && mcBB.valid);
    bool mcIsMoving = (mcMoving == 1 && hasMainchara);

    r->drawColor = 0xFFFFFFu;

    // ШАГ 1: Избавляемся от 100 тяжелых хэш-запросов за кадр!
    // Делаем один единственный линейный проход по памяти объекта.
    SnowFlakeCacheStruct flakes[25];
    memset(flakes, 0, sizeof(flakes));

    int32_t mapLen = (int32_t)hmlen(inst->selfArrayMap);
    for (int32_t i = 0; i < mapLen; i++) {
        int64_t k = inst->selfArrayMap[i].key;
        int32_t varId = (int32_t)(k >> 32);
        uint32_t packedIdx = (uint32_t)(k & 0xFFFFFFFF);

        int32_t xx = packedIdx / 32000;
        int32_t yy = packedIdx % 32000;

        if (xx >= 0 && xx < 5 && yy >= 0 && yy < 5) {
            int32_t fIdx = xx * 5 + yy;
            if (varId == snowfloorCache.snowx) flakes[fIdx].sxv = &inst->selfArrayMap[i].value;
            else if (varId == snowfloorCache.snowy) flakes[fIdx].syv = &inst->selfArrayMap[i].value;
            else if (varId == snowfloorCache.moveme) flakes[fIdx].mmv = &inst->selfArrayMap[i].value;
            else if (varId == snowfloorCache.dodraw) flakes[fIdx].ddv = &inst->selfArrayMap[i].value;
        }
    }

    // ШАГ 2: Обрабатываем снежинки напрямую через быстрые указатели
    for (int32_t i = 0; i < 25; i++) {
        if (!flakes[i].sxv || !flakes[i].syv || !flakes[i].mmv) continue;

        GMLReal snowx  = (flakes[i].sxv->type == RVALUE_REAL) ? flakes[i].sxv->real : RValue_toReal(*flakes[i].sxv);
        GMLReal snowy  = (flakes[i].syv->type == RVALUE_REAL) ? flakes[i].syv->real : RValue_toReal(*flakes[i].syv);
        GMLReal moveme = (flakes[i].mmv->type == RVALUE_REAL) ? flakes[i].mmv->real : RValue_toReal(*flakes[i].mmv);

        // Проверяем, находится ли снежинка в пределах экрана
        bool inView = (snowx >= vL && snowx <= vR && snowy >= vT && snowy <= vB);

        if (inView && flakes[i].ddv) {
            GMLReal dodraw = (flakes[i].ddv->type == RVALUE_REAL) ? flakes[i].ddv->real : RValue_toReal(*flakes[i].ddv);
            if (dodraw == 1.0) {
                // ИЛЛЮЗИЯ КРУГА ИЗ ДВУХ ПРЯМОУГОЛЬНИКОВ:
                // Вертикальная часть "крестика" (Ширина 3, Высота 5)
                r->vtable->drawRectangle(r, (float)snowx - 1.5f, (float)snowy - 2.5f,
                                         (float)snowx + 1.5f, (float)snowy + 2.5f,
                                         0xFFFFFFu, 1.0f, false);
                // Горизонтальная часть "крестика" (Ширина 5, Высота 3)
                r->vtable->drawRectangle(r, (float)snowx - 2.5f, (float)snowy - 1.5f,
                                         (float)snowx + 2.5f, (float)snowy + 1.5f,
                                         0xFFFFFFu, 1.0f, false);
            }
        }

        bool collided = false;
        if (canCollide) {
            float cx = (float)snowx;
            float cy = (float)snowy;
            float clx = (cx < mcBB.left) ? mcBB.left : (cx > mcBB.right ? mcBB.right : cx);
            float cly = (cy < mcBB.top)  ? mcBB.top  : (cy > mcBB.bottom? mcBB.bottom: cy);
            float dx = cx - clx;
            float dy = cy - cly;

            if (dx * dx + dy * dy < 4.0f) collided = true;
        }

        if (collided) {
            moveme = floorf(fast_rand_float() * 4.0f) + 2.0f;
        }

        if (moveme > 1.0) {
            if (mcIsMoving) {
                if (roomIs57 && needFlag64Set && snowfloorCache.gFlag >= 0) {
                    globalArraySet(ctx, snowfloorCache.gFlag, 64, RValue_makeReal(-1.0));
                    needFlag64Set = false;
                }

                if (mcBB.left   > snowx) snowx -= moveme;
                if (mcBB.right  < snowx) snowx += moveme;
                if (mcBB.top    > snowy) snowy -= moveme;
                if (mcBB.bottom < snowy) snowy += moveme;

                float rnd1 = fast_rand_float() * (float)moveme;
                float rnd2 = fast_rand_float() * (float)moveme;
                snowx += (rnd1 - (float)moveme / 2.0f) / 2.0f;
                snowy += (rnd2 - (float)moveme / 2.0f) / 2.0f;

                flakes[i].sxv->real = snowx; flakes[i].sxv->type = RVALUE_REAL;
                flakes[i].syv->real = snowy; flakes[i].syv->type = RVALUE_REAL;
            }

            moveme -= 1.0;
            flakes[i].mmv->real = moveme; flakes[i].mmv->type = RVALUE_REAL;
        } else if (collided) {
            flakes[i].mmv->real = moveme; flakes[i].mmv->type = RVALUE_REAL;
        }
    }
}

void NativeScripts_init(VMContext* ctx, [[maybe_unused]] Runner* runner) {
    DataWin* dw = ctx->dataWin;

    
    initWriterCache(ctx, dw);
    initMinihelixCache(dw);
    initBltParentCache(ctx);
    initBulletgenCache(ctx);
    initHpnameCache(dw);
    initChasefire2Cache(ctx, dw);
    initClawCache(dw);
    initDrakebodyCache(dw);
    initSwapperCache(ctx, dw);
    initIceteethCache(dw);
    initGettextCache(ctx);
    initFallobjCache(dw);
    initReadableCache(dw);
    initSpikesCache(ctx, dw);
    initFaceCache(ctx);
    initBarrierCache(dw);
    initAsghelixCache(dw);
    initOrangeCache(dw);
    initMercypartCache(dw);
    initPurplegradCache(dw);
    initAsgBodyCache(dw);
    initPartgenCache(dw);
    initTimeCache(ctx, dw);
    initMaincharaCache(ctx, dw);
    initLoopblgCache(ctx, dw);
    initXoxoCache(ctx, dw);
    initSpecialtileCache(ctx, dw);
    initDialoguerCache(ctx, dw);
    initOvrctrlCache(ctx, dw);
    initWaterdivotCache(dw);
    initSizeboneCache(ctx, dw);
    initSizeboneDrawCache(ctx, dw);
    initVapNewCache(ctx, dw);
    initSpearCache(dw);
    initCfireCache(dw);
    initGenfireCache(dw);
    initFirestormCache(ctx, dw);
    initBcCache(ctx, dw);
    initBouncersteamCache(dw);
    initBouncerightCache(ctx, dw);
    initCogsmallCache(dw);
    initHotlandBottomCache(dw);
    initHotlandRedCache(dw);
    initSpiderCache(dw);
    initRedpipevCache(dw);
    initLavaWaverCache(dw);
    initAntiWaverCache(dw);
    initPiperCache(dw);
    initHotlandRedXCache(dw);
    initBottomglowerCache(dw);
    initCounterscrollerCache(dw);
    initBgCoreCache(dw);
    initBluelaserCache(dw);
    initCoreLightstripCache(dw);
    initPlusbombCache(dw);
    initLeglineCache(ctx, dw);
    initMettbCache(ctx, dw);
    initRatingsCache(ctx, dw);
    initMettbossEventCache(dw);
    initPlusbombExplCache(dw);
    initFloweyPipeCache(ctx, dw);
    initFloweyBgdrawCache(dw);
    initFloweyMouthCache(ctx, dw);
    initFloweyEyeCache(dw);
    initBgpipeCache(dw);
    initVinesCache(dw);
    initFloweyLeftEyeCache(dw);
    initSidestalkCache(dw);
    initSpinbulletPrevCache(dw);
    initGigavinePrevCache(dw);
    initFloweyArmCache(dw);
    initFloweyTvCache(ctx, dw);
    initFloweyDmgCache(ctx, dw);
    initFogmakerCache(ctx, dw);
    initTopboneCache(ctx, dw);
    initCoolbusCache(ctx, dw);
    initFgWaterfallCache(dw);
    initWaterfallCache(dw);
    initGlowfly1Cache(dw);
    initGlowstoneCache(dw);
    initDarknesspuzzleCache(ctx, dw);
    initPuddleCache(ctx, dw);
    initUndyneSpearCache(dw);
    initPollenerCache(dw);
    initHotlandsignCache(dw);
    initParalavaCache(dw);
    initDummymissleCache(dw);
    initTemhandCache(dw);
    initBoxsinerCache(dw);
    initMaddumCache(ctx, dw);
    initHandgunCache(dw);
    initFleshfaceCache(dw);
    initDentataCache(dw);
    initWordbulletCache(dw);
    initPanCache(dw);
    initWhitesploderCache(dw);
    initDiscoballCache(dw);
    initMilkofhellCache(dw);
    initMettnewsCache(dw);
    initKitchenForcefieldCache(dw);
    initMettatonDress2Cache(dw);
    initMemoryheadCache(dw);
    initAfterimageAsrielCache(dw);
    initWrapshockCache(dw);
    initRoundedgeCache(ctx, dw);
    initAsrielBodyCache(ctx, dw);
    initMhdCache(dw);
    initStrangetangleCache(dw);
    initUltimatrailCache(dw);
    initLastbeamCache(ctx, dw);
    initAfinalBodyCache(ctx, dw);
    initHgBodyCache(dw);
    initGlowparticle1Cache(dw);
    initNormaldropCache(dw);
    initWaterpushrockCache(ctx, dw);
    initWaterboardpuzzle1Cache(dw);
    initWaterstarBgCache(dw);
    initSpeartileCache(dw);
    initDummybulletStepCache(ctx, dw);
    initDummyshotCache(dw);
    initDummymissleStepCache(dw);
    initConfettiCache(dw);
    initJarflyCache(dw);
    initMettatonnnWriterCache(ctx, dw);
    initBouncersteamCreateCache(dw);
    initChimesparkleCache(dw);
    initSugarbulletCache(dw);
    initSugarshotCollisionCache(dw);
    initMettEggbulletCache(dw);
    initSteamplume2Cache(dw);
    initMettnewsPartCache(dw);
    initSnowfloorCache(ctx, dw);

    
    VMBuiltins_register("scr_gettext", native_scr_gettext);

    
    registerNative("gml_Object_obj_ct_fallobj_Step_0", native_ctFallobj_Step0);
    registerNative("gml_Object_obj_ct_fallobj_Create_0", native_ctFallobj_Create0);

    
    registerNative("gml_Object_obj_base_writer_Draw_0", native_objBaseWriter_Draw0);
    
    

    
    registerNative("gml_Object_obj_dialoguer_Step_0", native_dialoguer_Step0);
    registerNative("gml_Object_obj_dialoguer_Draw_0", native_dialoguer_Draw0);
    registerNative("gml_Object_obj_face_Create_0", native_noop);

    
    registerNative("gml_Object_obj_overworldcontroller_Draw_0", native_overworldctrl_Draw0);

    
    registerNative("gml_Object_obj_waterdivot_test_Step_0", native_waterdivot_Step0);

    
    
    registerNative("gml_Object_blt_sizebone_Step_0", native_sizebone_Step0);
    registerNative("gml_Object_blt_sizebone_Step_2", native_sizebone_Step2);
    registerNative("gml_Object_blt_sizebone_Draw_0", native_sizebone_Draw0);

    
    registerNative("gml_Object_blt_minihelix_Step_0", native_bltMinihelix_Step0);
    registerNative("gml_Object_blt_parent_Step_2", native_bltParent_Step2);
    registerNative("gml_Object_obj_bulletgenparent_Step_2", native_bulletgenparent_Step2);
    registerNative("gml_Object_obj_hpname_Step_0", native_hpname_Step0);
    registerNative("gml_Object_blt_chasefire2_Step_0", native_chasefire2_Step0);
    registerNative("gml_Object_blt_chasefire2_Draw_0", native_drawSelfBorder);

    
    registerNative("gml_Object_obj_whtpxlgrav_Other_7", native_actionKillObject);
    registerNative("gml_Object_obj_whtpxlgrav_Other_0", native_actionKillObject);
    
    registerNative("gml_Object_obj_whtpxlgrav_Create_0", native_whtpxlgrav_Create0);
    
    registerNative("gml_Object_obj_vaporized_new_Draw_0", native_vapNew_Draw0);
    
    registerNative("gml_Object_obj_vaporized_Step_0", native_vaporized_Step0);

    
    registerNative("gml_Object_obj_readable_Step_0", native_readable_Step0);
    registerNative("gml_Object_obj_readablesolid_Step_0", native_readable_Step0);
    registerNative("gml_Object_obj_spikes_room_Step_0", native_spikesRoom_Step0);
    registerNative("gml_Object_obj_plotwall1_Step_0", native_plotwall1_Step0);
    
    
    registerNative("gml_Object_obj_finalbarrier_Draw_0", native_finalbarrier_Draw0);

    
    registerNative("gml_Object_obj_purplegradienter_Draw_0", native_purplegradienter_Draw0);
    registerNative("gml_Object_obj_sinefire_asghelix_Step_0", native_asghelix_Step0);
    registerNative("gml_Object_obj_asgorebulparent_Step_2", native_bltParent_Step2); 
    registerNative("gml_Object_obj_orangeparticle_Step_0", native_orangeparticle_Step0);
    registerNative("gml_Object_obj_mercybutton_part_Step_0", native_mercypart_Step0);

    
    registerNative("gml_Object_obj_battlecontroller_Draw_0", native_battlecontroller_Draw0);

    
    registerNative("gml_Object_obj_itemswapper_Draw_0", native_itemswapper_Draw0);

    
    registerNative("gml_Object_obj_iceteeth_Draw_0", native_iceteeth_Draw0);

    
    registerNative("gml_Object_blt_clawbullet_white_Draw_0", native_clawbullet_white_Draw0);
    registerNative("gml_Object_obj_drakebody_Draw_0", native_drakebody_Draw0);
    registerNative("gml_Object_blt_4sidebullet_Draw_0", native_4sidebullet_Draw0);

    
    registerNative("gml_Object_obj_asgoreb_body_Draw_0", native_asgoreb_body_Draw0);
    registerNative("gml_Object_obj_orangeparticlegen_Step_0", native_orangeparticlegen_Step0);
    
    registerNative("gml_Object_obj_time_Step_1", native_time_Step1);

    
    registerNative("gml_Object_obj_mainchara_Step_0", native_mainchara_Step0);
    registerNative("gml_Object_obj_screen_Step_1", native_screen_Step1);

    
    registerNative("gml_Object_blt_loopbulletgrow_Step_0", native_loopblg_Step0);
    registerNative("gml_Object_blt_loopbulletgrow_Step_2", native_loopblg_Step2);
    registerNative("gml_Object_blt_loopbulletgrow_Draw_0", native_loopblg_Draw0);

    
    registerNative("gml_Object_obj_xoxo_Step_0", native_xoxo_Step0);

    
    registerNative("gml_Object_obj_specialtile_Draw_0", native_specialtile_Draw0);
    registerNative("gml_Object_obj_specialtile_Alarm_0", native_specialtile_Alarm0);

    
    registerNative("gml_Object_obj_sided_fire_Draw_0", native_drawSelfBorder);
    registerNative("gml_Object_obj_sided_fire_Step_0", native_sidedfire_Step0);

    
    registerNative("gml_Object_obj_bouncersteam_Step_0", native_bouncersteam_Step0);
    registerNative("gml_Object_obj_bounceright_Step_0", native_bounceright_Step0);

    
    
    
    
    registerNative("gml_Object_obj_cogsmall_Draw_0", native_cogsmall_Draw0);
    registerNative("gml_Object_obj_hotland_bottomedge_Draw_0", native_hotlandBottom_Draw0);
    registerNative("gml_Object_obj_hotland_rededge_Draw_0", native_hotlandRed_Draw0);

    
    registerNative("gml_Object_obj_conveyor_parent_Draw_0", native_conveyor_Draw0);
    registerNative("gml_Object_obj_spiderstrand_Draw_0", native_spiderstrand_Draw0);
    registerNative("gml_Object_obj_redpipev_Draw_0", native_redpipev_Draw0);
    registerNative("gml_Object_obj_true_lavawaver_Draw_0", native_trueLavawaver_Draw0);
    registerNative("gml_Object_obj_true_antiwaver_Draw_0", native_trueAntiwaver_Draw0);

    
    
    registerNative("gml_Object_obj_piper_bluejet_Draw_0", native_piperBluejet_Draw0);
    registerNative("gml_Object_obj_piper_steam_Draw_0", native_piperSteam_Draw0);
    registerNative("gml_Object_obj_hotland_rededge_x_Draw_0", native_hotlandRedX_Draw0);
    registerNative("gml_Object_obj_bottomglower_Draw_0", native_bottomglower_Draw0);
    registerNative("gml_Object_obj_counterscroller_Draw_0", native_counterscroller_Draw0);
    registerNative("gml_Object_obj_backgrounder_core_Draw_0", native_backgrounderCore_Draw0);
    registerNative("gml_Object_obj_bluelaser_o_Draw_0", native_bluelaser_Draw0);

    
    registerNative("gml_Object_obj_core_lightstrip_m_Draw_0", native_coreLightstrip_Draw0);
    registerNative("gml_Object_obj_plusbomb_Draw_0", native_plusbomb_Draw0);
    registerNative("gml_Object_obj_legline_l_Draw_0", native_legline_l_Draw0);
    registerNative("gml_Object_obj_legline_r_Draw_0", native_legline_r_Draw0);
    registerNative("gml_Object_obj_mettb_body_Draw_0", native_mettbBody_Draw0);
    
    
    registerNative("gml_Object_obj_ratingsmaster_Draw_0", native_ratingsmaster_Draw0);

    
    registerNative("gml_Object_obj_mettboss_event_Draw_0", native_mettbossEvent_Draw0);
    registerNative("gml_Object_obj_plusbomb_explosion_Draw_0", native_plusbombExpl_Draw0);

    
    
    
    
    registerNative("gml_Object_obj_floweypipetest_Draw_0",   native_floweyPipe_Draw0);
    registerNative("gml_Object_obj_floweypipetest_2_Draw_0", native_floweyPipe2_Draw0);
    registerNative("gml_Object_obj_floweypipetest_3_Draw_0", native_floweyPipe3_Draw0);

    
    
    
    registerNative("gml_Object_obj_flowey_bgdraw_Draw_0",    native_floweyBgdraw_Draw0);
    registerNative("gml_Object_obj_floweyx_mouth_Draw_0",    native_floweyMouth_Draw0);
    registerNative("gml_Object_obj_floweyx_flipeye_Draw_0",  native_floweyEye_Draw0);

    
    
    registerNative("gml_Object_obj_bgpipe_Draw_0",           native_bgpipe_Draw0);
    registerNative("gml_Object_obj_vines_flowey_Draw_0",     native_vinesFlowey_Draw0);
    registerNative("gml_Object_obj_floweyx_lefteye_Draw_0",  native_floweyLeftEye_Draw0);

    
    registerNative("gml_Object_obj_sidestalk_Draw_0",                      native_sidestalk_Draw0);
    registerNative("gml_Object_obj_spinbullet_huge_gen_preview_Draw_0",    native_spinbulletPrev_Draw0);
    registerNative("gml_Object_obj_gigavine_preview_Draw_0",               native_gigavinePrev_Draw0);
    registerNative("gml_Object_obj_floweyarm_Draw_0",                      native_floweyArm_Draw0);
    registerNative("gml_Object_obj_floweyx_tv_Draw_0",                     native_floweyTv_Draw0);
    
    registerNative("gml_Object_obj_floweydmgwriter_Draw_0",                native_floweyDmgWriter_Draw0);

    
    registerNative("gml_Object_obj_fogmaker_Draw_0",     native_fogmaker_Draw0);
    registerNative("gml_Object_blt_topbone_Draw_0",      native_topbone_Draw0);
    registerNative("gml_Object_blt_coolbus_Draw_0",      native_coolbus_Draw0);

    
    registerNative("gml_Object_obj_foreground_waterfall_Draw_0", native_fgWaterfall_Draw0);
    registerNative("gml_Object_obj_waterfall_waterfall_Draw_0",  native_waterfallWaterfall_Draw0);
    registerNative("gml_Object_obj_brightwaterfall_Draw_0",      native_brightwaterfall_Draw0);
    registerNative("gml_Object_obj_glowfly1_Draw_0",             native_glowfly1_Draw0);
    registerNative("gml_Object_obj_glowstone_Draw_0",            native_glowstone_Draw0);
    registerNative("gml_Object_obj_darknesspuzzle_Draw_0",       native_darknesspuzzle_Draw0);
    registerNative("gml_Object_obj_puddle_Draw_0",               native_puddle_Draw0);
    registerNative("gml_Object_obj_undynespear_Draw_0",          native_undynespear_Draw0);
    registerNative("gml_Object_obj_pollener_Draw_0",             native_pollener_Draw0);
    registerNative("gml_Object_obj_hotlandsign_Draw_0",          native_hotlandsign_Draw0);
    registerNative("gml_Object_obj_hotlandparalava_Draw_0",      native_hotlandparalava_Draw0);

    
    
    registerNative("gml_Object_blt_sweatdrop_Draw_0",    native_sweatdrop_Draw0);
    registerNative("gml_Object_blt_uspear_Draw_0",       native_drawSelfBorder);
    registerNative("gml_Object_obj_maddummy_Draw_0",     native_drawSelfBorder);
    registerNative("gml_Object_blt_dummybullet_Draw_0",  native_drawSelfBorder);
    registerNative("gml_Object_blt_stalk2_Draw_0",       native_drawSelfBorder);
    
    registerNative("gml_Object_obj_maddum_drawer_Draw_0", native_maddumDrawer_Draw0);
    registerNative("gml_Object_blt_dummymissle_Draw_0",   native_dummymissle_Draw0);
    registerNative("gml_Object_blt_temhand_Draw_0",       native_temhand_Draw0);
    registerNative("gml_Object_obj_boxsiner_Draw_0",      native_boxsiner_Draw0);

    
    registerNative("gml_Object_obj_fleshface_Draw_0",        native_fleshface_Draw0);
    registerNative("gml_Object_obj_f_handgun_Draw_0",        native_handgun_Draw0);
    registerNative("gml_Object_obj_venus_pl_Draw_0",         native_venusPl_Draw0);
    registerNative("gml_Object_obj_dentata_full_Draw_0",     native_dentataFull_Draw0);
    registerNative("gml_Object_obj_6book_wordbullet_Draw_0", native_wordbullet_Draw0);
    registerNative("gml_Object_obj_6pan_Draw_0",             native_pan_Draw0);

    
    registerNative("gml_Object_obj_cfire_Step_0", native_cfire_Step0);
    registerNative("gml_Object_obj_genericfire_Step_0", native_genericfire_Step0);
    registerNative("gml_Object_obj_firestormgen_Draw_0", native_firestormgen_Draw0);

    
    
    
    
    
    registerNative("gml_Object_obj_asgorespear_Draw_0", native_asgorespear_Draw0);

    
    
    registerNative("gml_Object_obj_sidelava_Draw_0",    native_drawSelfBorder);
    registerNative("gml_Object_obj_orangefire_Draw_0",  native_drawSelfBorder);
    registerNative("gml_Object_obj_ropebul_Draw_0",     native_drawSelfBorder);
    
    registerNative("gml_Object_obj_amalgam_tooth_Draw_0", native_drawSelfBorder);

    
    
    registerNative("gml_Object_obj_memoryhead_body_Draw_0", native_memoryheadBody_Draw0);

    
    
    registerNative("gml_Object_obj_afterimage_asriel_Draw_0", native_afterimageAsriel_Draw0);
    registerNative("gml_Object_obj_wrapshock_Draw_0",         native_wrapshock_Draw0);
    registerNative("gml_Object_obj_roundedge_Draw_0",         native_roundedge_Draw0);

    
    
    
    
    
    
    registerNative("gml_Object_obj_asriel_body_Draw_0",       native_asrielBody_Draw0);

    
    
    
    
    
    registerNative("gml_Object_obj_afinal_body_Draw_0",     native_afinalBody_Draw0);
    registerNative("gml_Object_obj_ultimatrail_Draw_0",     native_ultimatrail_Draw0);
    registerNative("gml_Object_obj_strangetangle_Draw_0",   native_strangetangle_Draw0);
    registerNative("gml_Object_obj_lastbeam_Draw_0",        native_lastbeam_Draw0);
    registerNative("gml_Object_obj_mhd_Draw_0",             native_mhd_Draw0);
    
    registerNative("gml_Object_obj_ultimabullet_Draw_0",    native_ultimabullet_Draw0);

    
    
    
    
    registerNative("gml_Object_obj_hg_body_Draw_0",         native_hgBody_Draw0);

    
    
    registerNative("gml_Object_obj_normalplink_Step_0",      native_normalplink_Step0);
    registerNative("gml_Object_obj_glowparticle_1_Step_0",   native_glowparticle1_Step0);
    registerNative("gml_Object_obj_normaldrop_Step_0",       native_normaldrop_Step0);
    registerNative("gml_Object_obj_normaldrop_Other_11",     native_normaldrop_Other11);
    registerNative("gml_Object_obj_waterpushrock_Step_0",    native_waterpushrock_Step0);
    registerNative("gml_Object_obj_waterboardpuzzle1_Step_0",native_waterboardpuzzle1_Step0);

    
    registerNative("gml_Object_obj_waterstar_bg_Other_10",   native_waterstarBg_Other10);
    registerNative("gml_Object_obj_speartile_Step_0",        native_speartile_Step0);
    registerNative("gml_Object_blt_dummybullet_Step_0",      native_dummybullet_Step0);
    registerNative("gml_Object_blt_dummyshot_Collision_288", native_dummyshot_Collision288);
    registerNative("gml_Object_blt_dummymissle_Step_0",      native_dummymissle_Step0);

    
    registerNative("gml_Object_obj_confetti_Step_0",         native_confetti_Step0);
    registerNative("gml_Object_obj_jarfly_Step_0",           native_jarfly_Step0);
    registerNative("gml_Object_obj_mettatonnn_Writer_Draw_0",native_mettatonnnWriter_Draw0);
    registerNative("gml_Object_obj_bouncersteam_Create_0",   native_bouncersteam_Create0);
    registerNative("gml_Object_obj_bounceright_Alarm_1",     native_bounceright_Alarm1);

    
    
    
    registerNative("gml_Object_obj_chimesparkle_Step_0",            native_chimesparkle_Step0);
    registerNative("gml_Object_obj_sugarbullet_Step_0",             native_sugarbullet_Step0);
    registerNative("gml_Object_obj_sugarbullet_Collision_1187",     native_sugarbullet_Collision1187);
    registerNative("gml_Object_obj_milkofhell_shot_Step_0",         native_milkofhell_shot_Step0);
    registerNative("gml_Object_obj_milkofhell_shot_Other_10",       native_milkofhell_shot_Other10);
    registerNative("gml_Object_obj_milkofhell_shot_Collision_1187", native_milkofhell_shot_Collision1187);
    registerNative("gml_Object_obj_mett_eggbullet_Step_0",          native_mettEggbullet_Step0);

    
    registerNative("gml_Object_obj_steamplume2_Step_0",      native_steamplume2_Step0);
    registerNative("gml_Object_obj_mettnews_part_Step_2",    native_mettnewsPart_Step2);

    
    
    
    registerNative("gml_Object_obj_snowfloor_Draw_0",        native_snowfloor_Draw0);

    
    
    
    registerNative("gml_Object_obj_whitesploder_Draw_0",     native_whitesploder_Draw0);
    registerNative("gml_Object_obj_discoball_Draw_0",        native_discoball_Draw0);
    registerNative("gml_Object_obj_milkofhell_shot_Draw_0",  native_milkofhell_shot_Draw0);
    registerNative("gml_Object_obj_mettnews_ticker_Draw_0",  native_mettnews_ticker_Draw0);
    registerNative("gml_Object_obj_kitchenforcefield_Draw_0",native_kitchenForcefield_Draw0);
    registerNative("gml_Object_obj_mettaton_dress2_Draw_0",  native_mettaton_dress2_Draw0);

    
    
    registerNative("gml_Object_obj_time_Draw_76", native_time_Draw76);  
    registerNative("gml_Object_obj_time_Draw_77", native_time_Draw77);  
    registerNative("gml_Object_obj_time_Draw_64", native_noop);         
    registerNative("gml_Object_obj_time_Draw_75", native_noop);         

    fprintf(stderr, "NativeScripts: Registered %d native code overrides\n", (int32_t) shlen(nativeOverrideMap));
}