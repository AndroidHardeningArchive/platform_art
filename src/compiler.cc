// Copyright 2011 Google Inc. All Rights Reserved.

#include "compiler.h"

#include <sys/mman.h>

#include "assembler.h"
#include "class_linker.h"
#include "class_loader.h"
#include "dex_cache.h"
#include "jni_compiler.h"
#include "jni_internal.h"
#include "runtime.h"

extern bool oatCompileMethod(const art::Compiler& compiler, art::Method*, art::InstructionSet);

namespace art {

namespace arm {
  ByteArray* CreateAbstractMethodErrorStub();
  void ArmCreateInvokeStub(Method* method);
  ByteArray* ArmCreateResolutionTrampoline(bool is_static);
}
namespace x86 {
  ByteArray* CreateAbstractMethodErrorStub();
  void X86CreateInvokeStub(Method* method);
  ByteArray* X86CreateResolutionTrampoline(bool is_static);
}

Compiler::Compiler(InstructionSet insns) : instruction_set_(insns), jni_compiler_(insns),
    verbose_(false) {
  CHECK(!Runtime::Current()->IsStarted());
}

ByteArray* Compiler::CreateResolutionStub(InstructionSet instruction_set, bool is_static) {
  if (instruction_set == kX86) {
    return x86::X86CreateResolutionTrampoline(is_static);
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::ArmCreateResolutionTrampoline(is_static);
  }
}

ByteArray* Compiler::CreateAbstractMethodErrorStub(InstructionSet instruction_set) {
  if (instruction_set == kX86) {
    return x86::CreateAbstractMethodErrorStub();
  } else {
    CHECK(instruction_set == kArm || instruction_set == kThumb2);
    // Generates resolution stub using ARM instruction set
    return arm::CreateAbstractMethodErrorStub();
  }
}

void Compiler::CompileAll(const ClassLoader* class_loader) {
  DCHECK(!Runtime::Current()->IsStarted());
  Resolve(class_loader);
  Verify(class_loader);
  InitializeClassesWithoutClinit(class_loader);
  Compile(class_loader);
  SetCodeAndDirectMethods(class_loader);
}

void Compiler::CompileOne(Method* method) {
  DCHECK(!Runtime::Current()->IsStarted());
  const ClassLoader* class_loader = method->GetDeclaringClass()->GetClassLoader();
  Resolve(class_loader);
  Verify(class_loader);
  InitializeClassesWithoutClinit(class_loader);
  CompileMethod(method);
  SetCodeAndDirectMethods(class_loader);
}

void Compiler::Resolve(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    ResolveDexFile(class_loader, *dex_file);
  }
}

void Compiler::ResolveDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // Strings are easy, they always are simply resolved to literals in the same file
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t string_idx = 0; string_idx < dex_cache->NumStrings(); string_idx++) {
    class_linker->ResolveString(dex_file, string_idx, dex_cache);
  }

  // Class derived values are more complicated, they require the linker and loader.
  for (size_t type_idx = 0; type_idx < dex_cache->NumResolvedTypes(); type_idx++) {
    Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (klass == NULL) {
      Thread::Current()->ClearException();
    }
  }

  // Method and Field are the worst. We can't resolve without either
  // context from the code use (to disambiguate virtual vs direct
  // method and instance vs static field) or from class
  // definitions. While the compiler will resolve what it can as it
  // needs it, here we try to resolve fields and methods used in class
  // definitions, since many of them many never be referenced by
  // generated code.
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);

    // Note the class_data pointer advances through the headers,
    // static fields, instance fields, direct methods, and virtual
    // methods.
    const byte* class_data = dex_file.GetClassData(class_def);

    DexFile::ClassDataHeader header = dex_file.ReadClassDataHeader(&class_data);
    size_t num_static_fields = header.static_fields_size_;
    size_t num_instance_fields = header.instance_fields_size_;
    size_t num_direct_methods = header.direct_methods_size_;
    size_t num_virtual_methods = header.virtual_methods_size_;

    if (num_static_fields != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_static_fields; ++i) {
        DexFile::Field dex_field;
        dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
        Field* field = class_linker->ResolveField(dex_file, dex_field.field_idx_, dex_cache,
                                                  class_loader, true);
        if (field == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
    if (num_instance_fields != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_instance_fields; ++i) {
        DexFile::Field dex_field;
        dex_file.dexReadClassDataField(&class_data, &dex_field, &last_idx);
        Field* field = class_linker->ResolveField(dex_file, dex_field.field_idx_, dex_cache,
                                                  class_loader, false);
        if (field == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
    if (num_direct_methods != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_direct_methods; ++i) {
        DexFile::Method dex_method;
        dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
        Method* method = class_linker->ResolveMethod(dex_file, dex_method.method_idx_, dex_cache,
                                                     class_loader, true);
        if (method == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
    if (num_virtual_methods != 0) {
      uint32_t last_idx = 0;
      for (size_t i = 0; i < num_virtual_methods; ++i) {
        DexFile::Method dex_method;
        dex_file.dexReadClassDataMethod(&class_data, &dex_method, &last_idx);
        Method* method = class_linker->ResolveMethod(dex_file, dex_method.method_idx_, dex_cache,
                                                     class_loader, false);
        if (method == NULL) {
          Thread* self = Thread::Current();
          CHECK(self->IsExceptionPending());
          self->ClearException();
        }
      }
    }
  }
}

void Compiler::Verify(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    VerifyDexFile(class_loader, *dex_file);
  }
}

void Compiler::VerifyDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  dex_file.ChangePermissions(PROT_READ | PROT_WRITE);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass == NULL) {
      Thread* self = Thread::Current();
      CHECK(self->IsExceptionPending());
      self->ClearException();
      continue;
    }
    CHECK(klass->IsResolved()) << PrettyClass(klass);
    class_linker->VerifyClass(klass);

    if (klass->IsErroneous()) {
      // ClassLinker::VerifyClass throws, which isn't useful in the compiler.
      CHECK(Thread::Current()->IsExceptionPending());
      Thread::Current()->ClearException();
      // We want to try verification again at run-time, so move back into the resolved state.
      klass->SetStatus(Class::kStatusResolved);
    }

    CHECK(klass->IsVerified() || klass->IsResolved()) << PrettyClass(klass);
    CHECK(!Thread::Current()->IsExceptionPending()) << PrettyTypeOf(Thread::Current()->GetException());
  }
  dex_file.ChangePermissions(PROT_READ);
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    InitializeClassesWithoutClinit(class_loader, *dex_file);
  }
}

void Compiler::InitializeClassesWithoutClinit(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass != NULL) {
      class_linker->EnsureInitialized(klass, false);
    }
    // clear any class not found or verification exceptions
    Thread::Current()->ClearException();
  }

  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  for (size_t type_idx = 0; type_idx < dex_cache->NumResolvedTypes(); type_idx++) {
    Class* klass = class_linker->ResolveType(dex_file, type_idx, dex_cache, class_loader);
    if (klass == NULL) {
      Thread::Current()->ClearException();
    } else if (klass->IsInitialized()) {
      dex_cache->GetInitializedStaticStorage()->Set(type_idx, klass);
    }
  }
}

void Compiler::Compile(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    CompileDexFile(class_loader, *dex_file);
  }
}

void Compiler::CompileDexFile(const ClassLoader* class_loader, const DexFile& dex_file) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  for (size_t class_def_index = 0; class_def_index < dex_file.NumClassDefs(); class_def_index++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(class_def_index);
    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    Class* klass = class_linker->FindClass(descriptor, class_loader);
    if (klass == NULL) {
      // previous verification error will cause FindClass to throw
      Thread* self = Thread::Current();
      CHECK(self->IsExceptionPending());
      self->ClearException();
    } else {
      CompileClass(klass);
    }
  }
}

void Compiler::CompileClass(Class* klass) {
  for (size_t i = 0; i < klass->NumDirectMethods(); i++) {
    CompileMethod(klass->GetDirectMethod(i));
  }
  for (size_t i = 0; i < klass->NumVirtualMethods(); i++) {
    CompileMethod(klass->GetVirtualMethod(i));
  }
}

void Compiler::CompileMethod(Method* method) {
  if (method->IsNative()) {
    jni_compiler_.Compile(method);
    // unregister will install the stub to lookup via dlsym
    // TODO: this is only necessary for tests
    if (!method->IsRegistered()) {
      method->UnregisterNative();
    }
  } else if (method->IsAbstract()) {
    ByteArray* abstract_method_error_stub = Runtime::Current()->GetAbstractMethodErrorStubArray();
    if (instruction_set_ == kX86) {
      method->SetCodeArray(abstract_method_error_stub, kX86);
    } else {
      CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
      method->SetCodeArray(abstract_method_error_stub, kArm);
    }
  } else {
    oatCompileMethod(*this, method, kThumb2);
  }
  CHECK(method->GetCode() != NULL) << PrettyMethod(method);

  if (instruction_set_ == kX86) {
    art::x86::X86CreateInvokeStub(method);
  } else {
    CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
    // Generates invocation stub using ARM instruction set
    art::arm::ArmCreateInvokeStub(method);
  }
  CHECK(method->GetInvokeStub() != NULL);
}

void Compiler::SetCodeAndDirectMethods(const ClassLoader* class_loader) {
  const std::vector<const DexFile*>& class_path = ClassLoader::GetClassPath(class_loader);
  for (size_t i = 0; i != class_path.size(); ++i) {
    const DexFile* dex_file = class_path[i];
    CHECK(dex_file != NULL);
    SetCodeAndDirectMethodsDexFile(*dex_file);
  }
}

void Compiler::SetCodeAndDirectMethodsDexFile(const DexFile& dex_file) {
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  DexCache* dex_cache = class_linker->FindDexCache(dex_file);
  CodeAndDirectMethods* code_and_direct_methods = dex_cache->GetCodeAndDirectMethods();
  for (size_t i = 0; i < dex_cache->NumResolvedMethods(); i++) {
    Method* method = dex_cache->GetResolvedMethod(i);
    if ((method == NULL) || (method->IsStatic() && !method->GetDeclaringClass()->IsInitialized())) {
      ByteArray* res_trampoline = runtime->GetResolutionStubArray(method->IsStatic());
      if (instruction_set_ == kX86) {
        code_and_direct_methods->SetResolvedDirectMethodTrampoline(i, res_trampoline, kX86);
      } else {
        CHECK(instruction_set_ == kArm || instruction_set_ == kThumb2);
        code_and_direct_methods->SetResolvedDirectMethodTrampoline(i, res_trampoline, kArm);
      }
    } else if (method->IsDirect()) {
      code_and_direct_methods->SetResolvedDirectMethod(i, method);
    } else {
      // TODO: we currently leave the entry blank for resolved
      // non-direct methods.  we could put in an error stub.
    }
  }
}

}  // namespace art
