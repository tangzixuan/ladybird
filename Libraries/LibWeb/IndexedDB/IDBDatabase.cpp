/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBDatabasePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/IDBObjectStore.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBDatabase);

IDBDatabase::IDBDatabase(JS::Realm& realm, Database& db)
    : EventTarget(realm)
    , m_name(db.name())
    , m_associated_database(db)
{
    m_uuid = MUST(Crypto::generate_random_uuid());
    db.associate(*this);
    m_object_store_set = Vector<GC::Ref<ObjectStore>> { db.object_stores() };
}

IDBDatabase::~IDBDatabase() = default;

GC::Ref<IDBDatabase> IDBDatabase::create(JS::Realm& realm, Database& db)
{
    return realm.create<IDBDatabase>(realm, db);
}

void IDBDatabase::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBDatabase);
    Base::initialize(realm);
}

void IDBDatabase::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_object_store_set);
    visitor.visit(m_associated_database);
    visitor.visit(m_transactions);
}

void IDBDatabase::set_onabort(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::abort, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onabort()
{
    return event_handler_attribute(HTML::EventNames::abort);
}

void IDBDatabase::set_onerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

void IDBDatabase::set_onclose(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::close, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onclose()
{
    return event_handler_attribute(HTML::EventNames::close);
}

void IDBDatabase::set_onversionchange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::versionchange, event_handler);
}

WebIDL::CallbackType* IDBDatabase::onversionchange()
{
    return event_handler_attribute(HTML::EventNames::versionchange);
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-close
void IDBDatabase::close()
{
    // 1. Run close a database connection with this connection.
    close_a_database_connection(*this);
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-createobjectstore
WebIDL::ExceptionOr<GC::Ref<IDBObjectStore>> IDBDatabase::create_object_store(String const& name, IDBObjectStoreParameters const& options)
{
    auto& realm = this->realm();

    // 1. Let database be this's associated database.
    auto database = associated_database();

    // 2. Let transaction be database’s upgrade transaction if it is not null, or throw an "InvalidStateError" DOMException otherwise.
    auto transaction = database->upgrade_transaction();
    if (!transaction)
        return WebIDL::InvalidStateError::create(realm, "Upgrade transaction is null"_string);

    // 3. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while creating object store"_string);

    // 4. Let keyPath be options’s keyPath member if it is not undefined or null, or null otherwise.
    auto key_path = options.key_path;

    // 5. If keyPath is not null and is not a valid key path, throw a "SyntaxError" DOMException.
    if (key_path.has_value() && !is_valid_key_path(key_path.value()))
        return WebIDL::SyntaxError::create(realm, "Invalid key path"_string);

    // 6. If an object store named name already exists in database throw a "ConstraintError" DOMException.
    if (database->object_store_with_name(name))
        return WebIDL::ConstraintError::create(realm, "Object store already exists"_string);

    // 7. Let autoIncrement be options’s autoIncrement member.
    auto auto_increment = options.auto_increment;

    bool is_empty_key_path_or_sequence = key_path.has_value() && key_path.value().visit([](String const& value) -> bool { return value.is_empty(); }, [](Vector<String> const&) -> bool { return true; });

    // 8. If autoIncrement is true and keyPath is an empty string or any sequence (empty or otherwise), throw an "InvalidAccessError" DOMException.
    if (auto_increment && is_empty_key_path_or_sequence)
        return WebIDL::InvalidAccessError::create(realm, "Auto increment is true and key path is empty or sequence"_string);

    // 9. Let store be a new object store in database.
    //    Set the created object store's name to name.
    //    If autoIncrement is true, then the created object store uses a key generator.
    //    If keyPath is not null, set the created object store's key path to keyPath.
    auto object_store = ObjectStore::create(realm, database, name, auto_increment, key_path);

    // AD-HOC: Add newly created object store to this's object store set.
    add_to_object_store_set(object_store);

    // 10. Return a new object store handle associated with store and transaction.
    return IDBObjectStore::create(realm, object_store, *transaction);
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-objectstorenames
GC::Ref<HTML::DOMStringList> IDBDatabase::object_store_names()
{
    // 1. Let names be a list of the names of the object stores in this's object store set.
    Vector<String> names;
    for (auto const& object_store : this->object_store_set())
        names.append(object_store->name());

    // 2. Return the result (a DOMStringList) of creating a sorted name list with names.
    return create_a_sorted_name_list(realm(), names);
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-deleteobjectstore
WebIDL::ExceptionOr<void> IDBDatabase::delete_object_store(String const& name)
{
    auto& realm = this->realm();

    // 1. Let database be this's associated database.
    auto database = associated_database();

    // 2. Let transaction be database’s upgrade transaction if it is not null, or throw an "InvalidStateError" DOMException otherwise.
    auto transaction = database->upgrade_transaction();
    if (!transaction)
        return WebIDL::InvalidStateError::create(realm, "Upgrade transaction is null"_string);

    // 3. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
    if (!transaction->is_active())
        return WebIDL::TransactionInactiveError::create(realm, "Transaction is not active while deleting object store"_string);

    // 4. Let store be the object store named name in database, or throw a "NotFoundError" DOMException if none.
    auto store = database->object_store_with_name(name);
    if (!store)
        return WebIDL::NotFoundError::create(realm, "Object store not found while trying to delete"_string);

    // 5. Remove store from this's object store set.
    this->remove_from_object_store_set(*store);

    // FIXME: 6. If there is an object store handle associated with store and transaction, remove all entries from its index set.

    // 7. Destroy store.
    database->remove_object_store(*store);

    return {};
}

// https://w3c.github.io/IndexedDB/#dom-idbdatabase-transaction
WebIDL::ExceptionOr<GC::Ref<IDBTransaction>> IDBDatabase::transaction(Variant<String, Vector<String>> store_names, Bindings::IDBTransactionMode mode, IDBTransactionOptions options)
{
    auto& realm = this->realm();

    // 1. If a live upgrade transaction is associated with the connection, throw an "InvalidStateError" DOMException.
    auto database = associated_database();
    if (database->upgrade_transaction())
        return WebIDL::InvalidStateError::create(realm, "Upgrade transaction is live"_string);

    // 2. If this's close pending flag is true, then throw an "InvalidStateError" DOMException.
    if (close_pending())
        return WebIDL::InvalidStateError::create(realm, "Close pending"_string);

    // 3. Let scope be the set of unique strings in storeNames if it is a sequence, or a set containing one string equal to storeNames otherwise.
    Vector<String> scope;
    if (store_names.has<Vector<String>>()) {
        scope = store_names.get<Vector<String>>();
    } else {
        scope.append(store_names.get<String>());
    }

    // 4. If any string in scope is not the name of an object store in the connected database, throw a "NotFoundError" DOMException.
    for (auto const& store_name : scope) {
        if (!database->object_store_with_name(store_name))
            return WebIDL::NotFoundError::create(realm, "Provided object store names does not exist in database"_string);
    }

    // 5. If scope is empty, throw an "InvalidAccessError" DOMException.
    if (scope.is_empty())
        return WebIDL::InvalidAccessError::create(realm, "Scope is empty"_string);

    // 6. If mode is not "readonly" or "readwrite", throw a TypeError.
    if (mode != Bindings::IDBTransactionMode::Readonly && mode != Bindings::IDBTransactionMode::Readwrite)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Invalid transaction mode"_string };

    // 7. Let transaction be a newly created transaction with this connection, mode, options’ durability member, and the set of object stores named in scope.
    Vector<GC::Ref<ObjectStore>> scope_stores;
    for (auto const& store_name : scope) {
        auto store = database->object_store_with_name(store_name);
        scope_stores.append(*store);
    }

    auto transaction = IDBTransaction::create(realm, *this, mode, options.durability, scope_stores);

    // 8. Set transaction’s cleanup event loop to the current event loop.
    transaction->set_cleanup_event_loop(HTML::main_thread_event_loop());

    // 9. Return an IDBTransaction object representing transaction.
    return transaction;
}

}
