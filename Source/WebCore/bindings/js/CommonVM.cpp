/*
 * Copyright (C) 2016-2017 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "CommonVM.h"

#include "DOMWindow.h"
#include "DeprecatedGlobalSettings.h"
#include "Frame.h"
#include "ScriptController.h"
#include "WebCoreJSClientData.h"
#include <JavaScriptCore/HeapInlines.h>
#include <JavaScriptCore/MachineStackMarker.h>
#include <JavaScriptCore/VM.h>
#include <wtf/MainThread.h>
#include <wtf/text/AtomString.h>

#if PLATFORM(IOS_FAMILY)
#include "WebCoreThreadInternal.h"
#endif

namespace WebCore {

JSC::VM* g_commonVMOrNull = nullptr;

JSC::VM& commonVMSlow()
{
    ASSERT(isMainThread());
    ASSERT(!g_commonVMOrNull);

    ScriptController::initializeThreading();

    auto& vm = JSC::VM::create(JSC::LargeHeap).leakRef();

    g_commonVMOrNull = &vm;

    vm.heap.acquireAccess(); // At any time, we may do things that affect the GC.

#if PLATFORM(IOS_FAMILY)
    if (WebThreadIsEnabled())
        vm.apiLock().makeWebThreadAware();
    vm.setRunLoop(WebThreadRunLoop());
    vm.heap.machineThreads().addCurrentThread();
#endif

    vm.setGlobalConstRedeclarationShouldThrow(DeprecatedGlobalSettings::globalConstRedeclarationShouldThrow());

    JSVMClientData::initNormalWorld(&vm);

    return vm;
}

void destroyCommonVM() {
    if (g_commonVMOrNull) {
        delete g_commonVMOrNull;
        g_commonVMOrNull = nullptr;
    }
}

Frame* lexicalFrameFromCommonVM()
{
    if (auto* topCallFrame = commonVM().topCallFrame) {
        if (auto* globalObject = JSC::jsCast<JSDOMGlobalObject*>(topCallFrame->lexicalGlobalObject())) {
            if (auto* window = JSC::jsDynamicCast<JSDOMWindow*>(commonVM(), globalObject)) {
                if (auto* frame = window->wrapped().frame())
                    return frame;
            }
        }
    }
    return nullptr;
}

void addImpureProperty(const AtomString& propertyName)
{
    commonVM().addImpureProperty(propertyName);
}

} // namespace WebCore

