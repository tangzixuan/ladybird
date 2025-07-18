/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/CryptoKeyPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::Crypto {

GC_DEFINE_ALLOCATOR(CryptoKey);
GC_DEFINE_ALLOCATOR(CryptoKeyPair);

GC::Ref<CryptoKey> CryptoKey::create(JS::Realm& realm, InternalKeyData key_data)
{
    return realm.create<CryptoKey>(realm, move(key_data));
}

GC::Ref<CryptoKey> CryptoKey::create(JS::Realm& realm)
{
    return realm.create<CryptoKey>(realm);
}

CryptoKey::CryptoKey(JS::Realm& realm, InternalKeyData key_data)
    : PlatformObject(realm)
    , m_algorithm(Object::create(realm, nullptr))
    , m_usages(Object::create(realm, nullptr))
    , m_key_data(move(key_data))
{
}

CryptoKey::CryptoKey(JS::Realm& realm)
    : PlatformObject(realm)
    , m_algorithm(Object::create(realm, nullptr))
    , m_usages(Object::create(realm, nullptr))
    , m_key_data(MUST(ByteBuffer::create_uninitialized(0)))
{
}

CryptoKey::~CryptoKey()
{
    m_key_data.visit(
        [](ByteBuffer& data) { secure_zero(data.data(), data.size()); },
        [](auto& data) { secure_zero(reinterpret_cast<u8*>(&data), sizeof(data)); });
}

void CryptoKey::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CryptoKey);
    Base::initialize(realm);
}

void CryptoKey::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_algorithm);
    visitor.visit(m_usages);
}

void CryptoKey::set_usages(Vector<Bindings::KeyUsage> usages)
{
    m_key_usages = move(usages);
    auto& realm = this->realm();
    m_usages = JS::Array::create_from<Bindings::KeyUsage>(realm, m_key_usages.span(), [&](auto& key_usage) -> JS::Value {
        return JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(key_usage));
    });
}

String CryptoKey::algorithm_name() const
{
    if (m_algorithm_name.is_empty()) {
        auto name = MUST(m_algorithm->get("name"_fly_string));
        m_algorithm_name = MUST(name.to_string(vm()));
    }
    return m_algorithm_name;
}

GC::Ref<CryptoKeyPair> CryptoKeyPair::create(JS::Realm& realm, GC::Ref<CryptoKey> public_key, GC::Ref<CryptoKey> private_key)
{
    return realm.create<CryptoKeyPair>(realm, public_key, private_key);
}

CryptoKeyPair::CryptoKeyPair(JS::Realm& realm, GC::Ref<CryptoKey> public_key, GC::Ref<CryptoKey> private_key)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
    , m_public_key(public_key)
    , m_private_key(private_key)
{
}

void CryptoKeyPair::initialize(JS::Realm& realm)
{
    define_native_accessor(realm, "publicKey"_fly_string, public_key_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "privateKey"_fly_string, private_key_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);

    Base::initialize(realm);
}

void CryptoKeyPair::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_public_key);
    visitor.visit(m_private_key);
}

static JS::ThrowCompletionOr<CryptoKeyPair*> impl_from(JS::VM& vm)
{
    auto this_value = vm.this_value();
    JS::Object* this_object = nullptr;
    if (this_value.is_nullish())
        this_object = &vm.current_realm()->global_object();
    else
        this_object = TRY(this_value.to_object(vm));

    if (!is<CryptoKeyPair>(this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CryptoKeyPair");
    return static_cast<CryptoKeyPair*>(this_object);
}

JS_DEFINE_NATIVE_FUNCTION(CryptoKeyPair::public_key_getter)
{
    auto* impl = TRY(impl_from(vm));
    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->public_key(); }));
}

JS_DEFINE_NATIVE_FUNCTION(CryptoKeyPair::private_key_getter)
{
    auto* impl = TRY(impl_from(vm));
    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->private_key(); }));
}

WebIDL::ExceptionOr<void> CryptoKey::serialization_steps(HTML::TransferDataEncoder& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    auto& vm = this->vm();

    // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
    serialized.encode(m_type);

    // 2. Set serialized.[[Extractable]] to the [[extractable]] internal slot of value.
    serialized.encode(m_extractable);

    // 3. Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of value.
    auto serialized_algorithm = TRY(HTML::structured_serialize_internal(vm, m_algorithm, for_storage, memory));
    serialized.append(move(serialized_algorithm));

    // 4. Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value.
    auto serialized_usages = TRY(HTML::structured_serialize_internal(vm, m_usages, for_storage, memory));
    serialized.append(move(serialized_usages));

    // FIXME: 5. Set serialized.[[Handle]] to the [[handle]] internal slot of value.

    return {};
}

WebIDL::ExceptionOr<void> CryptoKey::deserialization_steps(HTML::TransferDataDecoder& serialized, HTML::DeserializationMemory& memory)
{
    auto& vm = this->vm();
    auto& realm = this->realm();

    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    m_type = serialized.decode<Bindings::KeyType>();

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    m_extractable = serialized.decode<bool>();

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    auto deserialized = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory));
    m_algorithm = deserialized.as_object();

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    deserialized = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory));
    m_usages = deserialized.as_object();

    // FIXME: 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].

    return {};
}

}
