/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLIFrameElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/DOMTokenList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Layout/NavigableContainerViewport.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLIFrameElement);

HTMLIFrameElement::HTMLIFrameElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : NavigableContainer(document, move(qualified_name))
{
}

HTMLIFrameElement::~HTMLIFrameElement() = default;

void HTMLIFrameElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLIFrameElement);
    Base::initialize(realm);
}

GC::Ptr<Layout::Node> HTMLIFrameElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::NavigableContainerViewport>(document(), *this, move(style));
}

void HTMLIFrameElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

void HTMLIFrameElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:process-the-iframe-attributes-2
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:process-the-iframe-attributes-3
    // Whenever an iframe element with a non-null content navigable has its srcdoc attribute set, changed, or removed,
    // the user agent must process the iframe attributes.
    // Similarly, whenever an iframe element with a non-null content navigable but with no srcdoc attribute specified
    // has its src attribute set, changed, or removed, the user agent must process the iframe attributes.
    if (m_content_navigable) {
        if (name == AttributeNames::srcdoc || (name == AttributeNames::src && !has_attribute(AttributeNames::srcdoc)))
            process_the_iframe_attributes();
    }

    if (name == HTML::AttributeNames::width || name == HTML::AttributeNames::height) {
        // FIXME: This should only invalidate the layout, not the style.
        invalidate_style(DOM::StyleInvalidationReason::HTMLIFrameElementGeometryChange);
    }

    if (name == HTML::AttributeNames::marginwidth || name == HTML::AttributeNames::marginheight) {
        if (auto* document = this->content_document_without_origin_check()) {
            if (auto* body_element = document->body())
                const_cast<HTMLElement*>(body_element)->set_needs_style_update(true);
        }
    }
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:html-element-post-connection-steps
void HTMLIFrameElement::post_connection()
{
    DOM::Document& document = as<DOM::Document>(shadow_including_root());

    // NOTE: The check for "not fully active" is to prevent a crash on the dom/nodes/node-appendchild-crash.html WPT test.
    if (!document.browsing_context() || !document.is_fully_active())
        return;

    // The iframe HTML element post-connection steps, given insertedNode, are:
    // 1. Create a new child navigable for insertedNode.
    MUST(create_new_child_navigable(GC::create_function(realm().heap(), [this] {
        // FIXME: 2. If insertedNode has a sandbox attribute, then parse the sandboxing directive given the attribute's
        //           value and insertedNode's iframe sandboxing flag set.

        // 3. Process the iframe attributes for insertedNode, with initialInsertion set to true.
        process_the_iframe_attributes(InitialInsertion::Yes);

        if (auto navigable = content_navigable()) {
            auto traversable = navigable->traversable_navigable();
            traversable->append_session_history_traversal_steps(GC::create_function(heap(), [this] {
                set_content_navigable_has_session_history_entry_and_ready_for_navigation();
            }));
        }
    })));
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#process-the-iframe-attributes
void HTMLIFrameElement::process_the_iframe_attributes(InitialInsertion initial_insertion)
{
    if (!content_navigable())
        return;

    // 1. If element's srcdoc attribute is specified, then:
    if (has_attribute(HTML::AttributeNames::srcdoc)) {
        // 1. Set element's current navigation was lazy loaded boolean to false.
        set_current_navigation_was_lazy_loaded(false);

        // 2. If the will lazy load element steps given element return true, then:
        if (will_lazy_load_element()) {
            // 1. Set element's lazy load resumption steps to the rest of this algorithm starting with the step labeled navigate to the srcdoc resource.
            set_lazy_load_resumption_steps([this]() {
                // 3. Navigate to the srcdoc resource: navigate an iframe or frame given element, about:srcdoc, the empty string, and the value of element's srcdoc attribute.
                navigate_an_iframe_or_frame(URL::about_srcdoc(), ReferrerPolicy::ReferrerPolicy::EmptyString, get_attribute(HTML::AttributeNames::srcdoc));

                // FIXME: The resulting Document must be considered an iframe srcdoc document.
            });

            // 2. Set element's current navigation was lazy loaded boolean to true.
            set_current_navigation_was_lazy_loaded(true);

            // 3. Start intersection-observing a lazy loading element for element.
            document().start_intersection_observing_a_lazy_loading_element(*this);

            // 4. Return.
            return;
        }

        // 3. Navigate to the srcdoc resource: navigate an iframe or frame given element, about:srcdoc, the empty string, and the value of element's srcdoc attribute.
        navigate_an_iframe_or_frame(URL::about_srcdoc(), ReferrerPolicy::ReferrerPolicy::EmptyString, get_attribute(HTML::AttributeNames::srcdoc));

        // FIXME: The resulting Document must be considered an iframe srcdoc document.

        return;
    }

    // 1. Let url be the result of running the shared attribute processing steps for iframe and frame elements given element and initialInsertion.
    auto url = shared_attribute_processing_steps_for_iframe_and_frame(initial_insertion);

    // 2. If url is null, then return.
    if (!url.has_value()) {
        return;
    }

    // 3. If url matches about:blank and initialInsertion is true, then:
    if (url_matches_about_blank(*url) && initial_insertion == InitialInsertion::Yes) {
        // 1. Run the iframe load event steps given element.
        run_iframe_load_event_steps(*this);

        // 2. Return.
        return;
    }

    // 4. Let referrerPolicy be the current state of element's referrerpolicy content attribute.
    auto referrer_policy = ReferrerPolicy::from_string(get_attribute_value(HTML::AttributeNames::referrerpolicy)).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // 5. Set element's current navigation was lazy loaded boolean to false.
    set_current_navigation_was_lazy_loaded(false);

    // 6. If the will lazy load element steps given element return true, then:
    if (will_lazy_load_element()) {
        // 1. Set element's lazy load resumption steps to the rest of this algorithm starting with the step labeled navigate.
        set_lazy_load_resumption_steps([this, url, referrer_policy]() {
            // 7. Navigate: navigate an iframe or frame given element, url, and referrerPolicy.
            navigate_an_iframe_or_frame(*url, referrer_policy);
        });

        // 2. Set element's current navigation was lazy loaded boolean to true.
        set_current_navigation_was_lazy_loaded(true);

        // 3. Start intersection-observing a lazy loading element for element.
        document().start_intersection_observing_a_lazy_loading_element(*this);

        // 4. Return.
        return;
    }

    // 7. Navigate: navigate an iframe or frame given element, url, and referrerPolicy.
    navigate_an_iframe_or_frame(*url, referrer_policy);
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:the-iframe-element-7
void HTMLIFrameElement::removed_from(DOM::Node* old_parent, DOM::Node& old_root)
{
    HTMLElement::removed_from(old_parent, old_root);

    // When an iframe element is removed from a document, the user agent must destroy the nested navigable of the element.
    destroy_the_child_navigable();
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#iframe-load-event-steps
void run_iframe_load_event_steps(HTML::HTMLIFrameElement& element)
{
    // FIXME: 1. Assert: element's content navigable is not null.
    if (!element.content_navigable()) {
        // FIXME: For some reason, we sometimes end up here in the middle of SunSpider.
        dbgln("FIXME: run_iframe_load_event_steps called with null nested browsing context");
        return;
    }

    // 2. Let childDocument be element's content navigable's active document.
    [[maybe_unused]] auto child_document = element.content_navigable()->active_document();

    // FIXME: 3. If childDocument has its mute iframe load flag set, then return.

    // FIXME: 4. Set childDocument's iframe load in progress flag.

    // 5. Fire an event named load at element.
    element.dispatch_event(DOM::Event::create(element.realm(), HTML::EventNames::load));

    // FIXME: 6. Unset childDocument's iframe load in progress flag.
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLIFrameElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

bool HTMLIFrameElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name == HTML::AttributeNames::frameborder;
}

void HTMLIFrameElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);

    // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:attr-iframe-frameborder
    // When an iframe element has a frameborder attribute whose value, when parsed using the rules for parsing integers,
    // is zero or an error, the user agent is expected to have presentational hints setting the element's
    // 'border-top-width', 'border-right-width', 'border-bottom-width', and 'border-left-width' properties to zero.
    if (auto frameborder_attribute = get_attribute(HTML::AttributeNames::frameborder); frameborder_attribute.has_value()) {
        auto frameborder = parse_integer(*frameborder_attribute);
        if (!frameborder.has_value() || frameborder == 0) {
            auto zero = CSS::LengthStyleValue::create(CSS::Length::make_px(0));
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderTopWidth, zero);
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderRightWidth, zero);
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderBottomWidth, zero);
            cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::BorderLeftWidth, zero);
        }
    }
}

// https://html.spec.whatwg.org/multipage/iframe-embed-object.html#dom-iframe-sandbox
GC::Ref<DOM::DOMTokenList> HTMLIFrameElement::sandbox()
{
    // The sandbox IDL attribute must reflect the sandbox content attribute.
    if (!m_sandbox)
        m_sandbox = DOM::DOMTokenList::create(*this, HTML::AttributeNames::sandbox);
    return *m_sandbox;
}

void HTMLIFrameElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visit_lazy_loading_element(visitor);
    visitor.visit(m_sandbox);
}

void HTMLIFrameElement::set_current_navigation_was_lazy_loaded(bool value)
{
    m_current_navigation_was_lazy_loaded = value;

    // An iframe element whose current navigation was lazy loaded boolean is false potentially delays the load event.
    // https://html.spec.whatwg.org/multipage/iframe-embed-object.html#the-iframe-element:potentially-delays-the-load-event
    set_potentially_delays_the_load_event(!value);
}

}
