#include "il2cpp-config.h"
#include "metadata/GenericMetadata.h"
#include "metadata/GenericMethod.h"
#include "metadata/GenericSharing.h"
#include "metadata/Il2CppGenericMethodCompare.h"
#include "metadata/Il2CppGenericMethodHash.h"
#include "os/Mutex.h"
#include "utils/Memory.h"
#include "vm/Class.h"
#include "vm/Exception.h"
#include "vm/GenericClass.h"
#include "vm/MetadataAlloc.h"
#include "vm/MetadataCache.h"
#include "vm/MetadataLock.h"
#include "vm/Method.h"
#include "vm/Runtime.h"
#include "vm/Type.h"
#include "utils/Il2CppHashMap.h"
#include "il2cpp-class-internals.h"
#include "il2cpp-runtime-metadata.h"
#include "il2cpp-runtime-stats.h"
#include <string>

#include "hybridclr/metadata/MetadataUtil.h"
#include "hybridclr/metadata/MetadataModule.h"
#include "hybridclr/interpreter/InterpreterModule.h"

using il2cpp::metadata::GenericMetadata;
using il2cpp::metadata::GenericSharing;
using il2cpp::os::FastAutoLock;
using il2cpp::vm::Class;
using il2cpp::vm::GenericClass;
using il2cpp::vm::MetadataCalloc;
using il2cpp::vm::MetadataCache;
using il2cpp::vm::Method;
using il2cpp::vm::Runtime;
using il2cpp::vm::Type;

namespace il2cpp
{
namespace metadata
{
    typedef Il2CppReaderWriterLockedHashMap<const Il2CppGenericMethod*, MethodInfo*, Il2CppGenericMethodHash, Il2CppGenericMethodCompare> Il2CppGenericMethodMap;
    static Il2CppGenericMethodMap s_GenericMethodMap;
    static Il2CppGenericMethodMap s_PendingGenericMethodMap;

    static void AGenericMethodWhichIsTooDeeplyNestedWasInvoked()
    {
        vm::Exception::Raise(vm::Exception::GetMaxmimumNestedGenericsException());
    }

    const MethodInfo* GenericMethod::GetGenericVirtualMethod(const MethodInfo* vtableSlotMethod, const MethodInfo* genericVirtualMethod)
    {
        IL2CPP_NOT_IMPLEMENTED_NO_ASSERT(GetGenericVirtualMethod, "We should only do the following slow method lookup once and then cache on type itself.");

        const Il2CppGenericInst* classInst = NULL;
        if (vtableSlotMethod->is_inflated)
        {
            classInst = vtableSlotMethod->genericMethod->context.class_inst;
            vtableSlotMethod = vtableSlotMethod->genericMethod->methodDefinition;
        }

        Il2CppGenericMethod gmethod = { 0 };
        gmethod.methodDefinition = vtableSlotMethod;
        gmethod.context.class_inst = classInst;
        gmethod.context.method_inst = genericVirtualMethod->genericMethod->context.method_inst;

        return metadata::GenericMethod::GetMethod(&gmethod, true);
    }

    const MethodInfo* GenericMethod::GetMethod(const Il2CppGenericMethod* gmethod, bool copyMethodPtr)
    {
        // This can be NULL only when we have hit the generic recursion depth limit.
        if (gmethod == NULL)
        {
            MethodInfo* newMethod = (MethodInfo*)MetadataCalloc(1, sizeof(MethodInfo));
            newMethod->methodPointer = AGenericMethodWhichIsTooDeeplyNestedWasInvoked;
            return newMethod;
        }

        // First check for an already constructed generic method using the shared/reader lock
        MethodInfo* existingMethod;
        if (s_GenericMethodMap.TryGet(gmethod, &existingMethod))
            return existingMethod;

        return CreateMethodLocked(gmethod, copyMethodPtr);
    }

    const MethodInfo* GenericMethod::CreateMethodLocked(const Il2CppGenericMethod* gmethod, bool copyMethodPtr)
    {
        // We need to inflate a new generic method, take the metadata mutex
        // All code below this point can and does assume mutual exclusion
        FastAutoLock lock(&il2cpp::vm::g_MetadataLock);

        // Recheck the s_GenericMethodMap in case there was a race to add this generic method
        MethodInfo* existingMethod;
        if (s_GenericMethodMap.TryGet(gmethod, &existingMethod))
            return existingMethod;

        // GetMethodLocked may be called recursively, we keep tracking of pending inflations
        if (s_PendingGenericMethodMap.TryGet(gmethod, &existingMethod))
            return existingMethod;

        if (copyMethodPtr)
        {
            Il2CppGenericMethod *newGMethod = vm::MetadataAllocGenericMethod();
            newGMethod->methodDefinition = gmethod->methodDefinition;
            newGMethod->context = gmethod->context;
            gmethod = newGMethod;
        }

        const MethodInfo* methodDefinition = gmethod->methodDefinition;
        Il2CppClass* declaringClass = methodDefinition->klass;
        if (gmethod->context.class_inst)
        {
            Il2CppGenericClass* genericClassDeclaringType = GenericMetadata::GetGenericClass(methodDefinition->klass, gmethod->context.class_inst);
            declaringClass = GenericClass::GetClass(genericClassDeclaringType);

            // we may fail if we cannot construct generic type
            if (!declaringClass)
                return NULL;
        }
        MethodInfo* newMethod = (MethodInfo*)MetadataCalloc(1, sizeof(MethodInfo));

        // we set the pending generic method map here because the initialization may recurse and try to retrieve the same generic method
        // this is safe because we *always* take the lock when retrieving the MethodInfo from a generic method.
        // if we move lock to only if MethodInfo needs constructed then we need to revisit this since we could return a partially initialized MethodInfo
        s_PendingGenericMethodMap.Add(gmethod, newMethod);

        newMethod->klass = declaringClass;
        newMethod->flags = methodDefinition->flags;
        newMethod->iflags = methodDefinition->iflags;
        newMethod->slot = methodDefinition->slot;
        newMethod->name = methodDefinition->name;
        newMethod->is_generic = false;
        newMethod->is_inflated = true;
        newMethod->token = methodDefinition->token;

        newMethod->return_type = GenericMetadata::InflateIfNeeded(methodDefinition->return_type, &gmethod->context, true);

        newMethod->parameters_count = methodDefinition->parameters_count;
        newMethod->parameters = GenericMetadata::InflateParameters(methodDefinition->parameters, methodDefinition->parameters_count, &gmethod->context, true);

        newMethod->genericMethod = gmethod;

        if (!gmethod->context.method_inst)
        {
            if (methodDefinition->is_generic)
                newMethod->is_generic = true;

            if (!declaringClass->generic_class)
            {
                newMethod->genericContainerHandle = methodDefinition->genericContainerHandle;
            }

            newMethod->methodMetadataHandle = methodDefinition->methodMetadataHandle;
        }
        else
        {
            // we only need RGCTX for generic instance methods
            newMethod->rgctx_data = GenericMetadata::InflateRGCTX(gmethod->methodDefinition->klass->image, gmethod->methodDefinition->token, &gmethod->context);
        }

        newMethod->invoker_method = MetadataCache::GetInvokerMethodPointer(methodDefinition, &gmethod->context);
        newMethod->methodPointer = MetadataCache::GetMethodPointer(methodDefinition, &gmethod->context, true, true);

        bool isAdjustorThunkMethod = newMethod->klass->valuetype && hybridclr::metadata::IsInstanceMethod(newMethod);
        if (newMethod->methodPointer == nullptr)
        {
            if ((hybridclr::metadata::IsInterpreterMethod(newMethod) || hybridclr::metadata::MetadataModule::IsImplementedByInterpreter(newMethod)))
            {
                newMethod->invoker_method = hybridclr::interpreter::InterpreterModule::GetMethodInvoker(newMethod);
                newMethod->methodPointerCallByInterp = hybridclr::interpreter::InterpreterModule::GetMethodPointer(newMethod);
                if (isAdjustorThunkMethod)
                {
                    newMethod->virtualMethodPointerCallByInterp = hybridclr::interpreter::InterpreterModule::GetAdjustThunkMethodPointer(newMethod);
                }
                else
                {
                    newMethod->virtualMethodPointerCallByInterp = newMethod->methodPointerCallByInterp;
                }
                newMethod->methodPointer = newMethod->virtualMethodPointerCallByInterp;
                newMethod->isInterpterImpl = true;
                newMethod->initInterpCallMethodPointer = true;
            }
            else
            {
                // not init anything
            }
        }
        else
        {
            newMethod->virtualMethodPointerCallByInterp = newMethod->methodPointer;
            newMethod->methodPointerCallByInterp = isAdjustorThunkMethod ? MetadataCache::GetMethodPointer(methodDefinition, &gmethod->context, false, true) : newMethod->methodPointer;
            newMethod->initInterpCallMethodPointer = true;
        }

        ++il2cpp_runtime_stats.inflated_method_count;


        // The generic method is fully created,
        // Update the generic method map, this needs to take an exclusive lock
        // **** This must happen with the metadata lock held and be released before the metalock is released ****
        // **** This prevents deadlocks and ensures that there is no race condition
        // **** creating a new method adding it to s_GenericMethodMap and removing it from s_PendingGenericMethodMap
        s_GenericMethodMap.Add(gmethod, newMethod);

        // Remove the method from the pending table
        s_PendingGenericMethodMap.Remove(gmethod);

        return newMethod;
    }

    const Il2CppGenericContext* GenericMethod::GetContext(const Il2CppGenericMethod* gmethod)
    {
        return &gmethod->context;
    }

    static std::string FormatGenericArguments(const Il2CppGenericInst* inst)
    {
        std::string output;
        if (inst)
        {
            output.append("<");
            for (size_t i = 0; i < inst->type_argc; ++i)
            {
                if (i != 0)
                    output.append(", ");
                output.append(Type::GetName(inst->type_argv[i], IL2CPP_TYPE_NAME_FORMAT_FULL_NAME));
            }
            output.append(">");
        }

        return output;
    }

    std::string GenericMethod::GetFullName(const Il2CppGenericMethod* gmethod)
    {
        const MethodInfo* method = gmethod->methodDefinition;
        std::string output;
        output.append(Type::GetName(&gmethod->methodDefinition->klass->byval_arg, IL2CPP_TYPE_NAME_FORMAT_FULL_NAME));
        output.append(FormatGenericArguments(gmethod->context.class_inst));
        output.append("::");
        output.append(Method::GetName(method));
        output.append(FormatGenericArguments(gmethod->context.method_inst));

        return output;
    }

    void GenericMethod::ClearStatics()
    {
        s_GenericMethodMap.Clear();
    }
} /* namespace vm */
} /* namespace il2cpp */
