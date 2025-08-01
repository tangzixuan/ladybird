/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/Bitmap.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/WOFF/Loader.h>
#include <LibGfx/Font/WOFF2/Loader.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Fetch.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CSSColorValue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Response.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/MimeSniff/Resource.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <math.h>
#include <stdio.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleComputer);
GC_DEFINE_ALLOCATOR(FontLoader);

struct FontFaceKey {
    NonnullRawPtr<FlyString const> family_name;
    int weight { 0 };
    int slope { 0 };
};

}

namespace AK {

namespace Detail {

template<>
inline constexpr bool IsHashCompatible<Web::CSS::FontFaceKey, Web::CSS::OwnFontFaceKey> = true;
template<>
inline constexpr bool IsHashCompatible<Web::CSS::OwnFontFaceKey, Web::CSS::FontFaceKey> = true;

}

template<>
struct Traits<Web::CSS::FontFaceKey> : public DefaultTraits<Web::CSS::FontFaceKey> {
    static unsigned hash(Web::CSS::FontFaceKey const& key) { return pair_int_hash(key.family_name->hash(), pair_int_hash(key.weight, key.slope)); }
};

template<>
struct Traits<Web::CSS::OwnFontFaceKey> : public DefaultTraits<Web::CSS::OwnFontFaceKey> {
    static unsigned hash(Web::CSS::OwnFontFaceKey const& key) { return pair_int_hash(key.family_name.hash(), pair_int_hash(key.weight, key.slope)); }
};

}

namespace Web::CSS {

CSSStyleProperties const& MatchingRule::declaration() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).declaration();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).declaration();
    VERIFY_NOT_REACHED();
}

SelectorList const& MatchingRule::absolutized_selectors() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).absolutized_selectors();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).parent_style_rule().absolutized_selectors();
    VERIFY_NOT_REACHED();
}

FlyString const& MatchingRule::qualified_layer_name() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).qualified_layer_name();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).parent_style_rule().qualified_layer_name();
    VERIFY_NOT_REACHED();
}

OwnFontFaceKey::OwnFontFaceKey(FontFaceKey const& other)
    : family_name(other.family_name)
    , weight(other.weight)
    , slope(other.slope)
{
}

OwnFontFaceKey::operator FontFaceKey() const
{
    return FontFaceKey {
        family_name,
        weight,
        slope
    };
}

[[nodiscard]] bool OwnFontFaceKey::operator==(FontFaceKey const& other) const
{
    return family_name == other.family_name
        && weight == other.weight
        && slope == other.slope;
}

static DOM::Element const* element_to_inherit_style_from(DOM::Element const*, Optional<CSS::PseudoElement>);

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font(16)->pixel_metrics())
    , m_root_element_font_metrics(m_default_font_metrics)
{
    m_ancestor_filter = make<CountingBloomFilter<u8, 14>>();
    m_qualified_layer_names_in_order.append({});
}

StyleComputer::~StyleComputer() = default;

void StyleComputer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_loaded_fonts);
    visitor.visit(m_user_style_sheet);
}

FontLoader::FontLoader(StyleComputer& style_computer, GC::Ptr<CSSStyleSheet> parent_style_sheet, FlyString family_name, Vector<Gfx::UnicodeRange> unicode_ranges, Vector<URL> urls, Function<void(RefPtr<Gfx::Typeface const>)> on_load)
    : m_style_computer(style_computer)
    , m_parent_style_sheet(parent_style_sheet)
    , m_family_name(move(family_name))
    , m_unicode_ranges(move(unicode_ranges))
    , m_urls(move(urls))
    , m_on_load(move(on_load))
{
}

FontLoader::~FontLoader() = default;

void FontLoader::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_style_computer);
    visitor.visit(m_parent_style_sheet);
    visitor.visit(m_fetch_controller);
}

bool FontLoader::is_loading() const
{
    return m_fetch_controller && !m_vector_font;
}

RefPtr<Gfx::Font const> FontLoader::font_with_point_size(float point_size)
{
    if (!m_vector_font) {
        if (!m_fetch_controller)
            start_loading_next_url();
        return nullptr;
    }
    return m_vector_font->font(point_size);
}

void FontLoader::start_loading_next_url()
{
    // FIXME: Load local() fonts somehow.
    if (m_fetch_controller && m_fetch_controller->state() == Fetch::Infrastructure::FetchController::State::Ongoing)
        return;
    if (m_urls.is_empty())
        return;

    // https://drafts.csswg.org/css-fonts-4/#fetch-a-font
    // To fetch a font given a selected <url> url for @font-face rule, fetch url, with stylesheet being rule’s parent
    // CSS style sheet, destination "font", CORS mode "cors", and processResponse being the following steps given
    // response res and null, failure or a byte stream stream:
    auto style_sheet_or_document = m_parent_style_sheet ? StyleSheetOrDocument { *m_parent_style_sheet } : StyleSheetOrDocument { m_style_computer->document() };
    auto maybe_fetch_controller = fetch_a_style_resource(m_urls.take_first(), style_sheet_or_document, Fetch::Infrastructure::Request::Destination::Font, CorsMode::Cors,
        [loader = this](auto response, auto stream) {
            // 1. If stream is null, return.
            // 2. Load a font from stream according to its type.

            // NB: We need to fetch the next source if this one fails to fetch OR decode. So, first try to decode it.
            RefPtr<Gfx::Typeface const> typeface;
            if (auto* bytes = stream.template get_pointer<ByteBuffer>()) {
                if (auto maybe_typeface = loader->try_load_font(response, *bytes); !maybe_typeface.is_error())
                    typeface = maybe_typeface.release_value();
            }

            if (!typeface) {
                // NB: If we have other sources available, try the next one.
                if (loader->m_urls.is_empty()) {
                    loader->font_did_load_or_fail(nullptr);
                } else {
                    loader->m_fetch_controller = nullptr;
                    loader->start_loading_next_url();
                }
            } else {
                loader->font_did_load_or_fail(move(typeface));
            }
        });

    if (maybe_fetch_controller.is_error()) {
        font_did_load_or_fail(nullptr);
    } else {
        m_fetch_controller = maybe_fetch_controller.release_value();
    }
}

void FontLoader::font_did_load_or_fail(RefPtr<Gfx::Typeface const> typeface)
{
    if (typeface) {
        m_vector_font = typeface.release_nonnull();
        m_style_computer->did_load_font(m_family_name);
        if (m_on_load)
            m_on_load(m_vector_font);
    } else {
        if (m_on_load)
            m_on_load(nullptr);
    }
    m_fetch_controller = nullptr;
}

ErrorOr<NonnullRefPtr<Gfx::Typeface const>> FontLoader::try_load_font(Fetch::Infrastructure::Response const& response, ByteBuffer const& bytes)
{
    // FIXME: This could maybe use the format() provided in @font-face as well, since often the mime type is just application/octet-stream and we have to try every format
    auto mime_type = response.header_list()->extract_mime_type();
    if (!mime_type.has_value() || !mime_type->is_font()) {
        mime_type = MimeSniff::Resource::sniff(bytes, MimeSniff::SniffingConfiguration { .sniffing_context = MimeSniff::SniffingContext::Font });
    }
    if (mime_type.has_value()) {
        if (mime_type->essence() == "font/ttf"sv || mime_type->essence() == "application/x-font-ttf"sv || mime_type->essence() == "font/otf"sv) {
            if (auto result = Gfx::Typeface::try_load_from_temporary_memory(bytes); !result.is_error()) {
                return result;
            }
        }
        if (mime_type->essence() == "font/woff"sv || mime_type->essence() == "application/font-woff"sv) {
            if (auto result = WOFF::try_load_from_bytes(bytes); !result.is_error()) {
                return result;
            }
        }
        if (mime_type->essence() == "font/woff2"sv || mime_type->essence() == "application/font-woff2"sv) {
            if (auto result = WOFF2::try_load_from_bytes(bytes); !result.is_error()) {
                return result;
            }
        }
    }

    return Error::from_string_literal("Automatic format detection failed");
}

struct StyleComputer::MatchingFontCandidate {
    FontFaceKey key;
    Variant<FontLoaderList*, Gfx::Typeface const*> loader_or_typeface;

    [[nodiscard]] RefPtr<Gfx::FontCascadeList const> font_with_point_size(float point_size) const
    {
        auto font_list = Gfx::FontCascadeList::create();
        if (auto* loader_list = loader_or_typeface.get_pointer<FontLoaderList*>(); loader_list) {
            for (auto const& loader : **loader_list) {
                if (auto font = loader->font_with_point_size(point_size); font)
                    font_list->add(*font, loader->unicode_ranges());
            }
            return font_list;
        }

        font_list->add(loader_or_typeface.get<Gfx::Typeface const*>()->font(point_size));
        return font_list;
    }
};

static CSSStyleSheet& default_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String default_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), default_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& quirks_mode_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String quirks_mode_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), quirks_mode_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& mathml_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String mathml_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), mathml_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& svg_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String svg_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), svg_stylesheet_source));
    }
    return *sheet;
}

Optional<String> StyleComputer::user_agent_style_sheet_source(StringView name)
{
    extern String default_stylesheet_source;
    extern String quirks_mode_stylesheet_source;
    extern String mathml_stylesheet_source;
    extern String svg_stylesheet_source;

    if (name == "CSS/Default.css"sv)
        return default_stylesheet_source;
    if (name == "CSS/QuirksMode.css"sv)
        return quirks_mode_stylesheet_source;
    if (name == "MathML/Default.css"sv)
        return mathml_stylesheet_source;
    if (name == "SVG/Default.css"sv)
        return svg_stylesheet_source;
    return {};
}

template<typename Callback>
void StyleComputer::for_each_stylesheet(CascadeOrigin cascade_origin, Callback callback) const
{
    if (cascade_origin == CascadeOrigin::UserAgent) {
        callback(default_stylesheet(), {});
        if (document().in_quirks_mode())
            callback(quirks_mode_stylesheet(), {});
        callback(mathml_stylesheet(), {});
        callback(svg_stylesheet(), {});
    }
    if (cascade_origin == CascadeOrigin::User) {
        if (m_user_style_sheet)
            callback(*m_user_style_sheet, {});
    }
    if (cascade_origin == CascadeOrigin::Author) {
        document().for_each_active_css_style_sheet([&](auto& sheet, auto shadow_root) {
            callback(sheet, shadow_root);
        });
    }
}

RuleCache const* StyleComputer::rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, Optional<FlyString const> qualified_layer_name, GC::Ptr<DOM::ShadowRoot const> shadow_root) const
{
    auto const* rule_caches_for_document_and_shadow_roots = [&]() -> RuleCachesForDocumentAndShadowRoots const* {
        switch (cascade_origin) {
        case CascadeOrigin::Author:
            return m_author_rule_cache;
        case CascadeOrigin::User:
            return m_user_rule_cache;
        case CascadeOrigin::UserAgent:
            return m_user_agent_rule_cache;
        default:
            VERIFY_NOT_REACHED();
        }
    }();
    auto const* rule_caches_by_layer = [&]() -> RuleCaches const* {
        if (shadow_root)
            return rule_caches_for_document_and_shadow_roots->for_shadow_roots.get(*shadow_root).value_or(nullptr);
        return &rule_caches_for_document_and_shadow_roots->for_document;
    }();
    if (!rule_caches_by_layer)
        return nullptr;
    if (!qualified_layer_name.has_value())
        return &rule_caches_by_layer->main;
    return rule_caches_by_layer->by_layer.get(*qualified_layer_name).value_or(nullptr);
}

[[nodiscard]] static bool filter_namespace_rule(Optional<FlyString> const& element_namespace_uri, MatchingRule const& rule)
{
    // FIXME: Filter out non-default namespace using prefixes
    if (rule.default_namespace.has_value() && element_namespace_uri != rule.default_namespace)
        return false;
    return true;
}

RuleCache const& StyleComputer::get_pseudo_class_rule_cache(PseudoClass pseudo_class) const
{
    build_rule_cache_if_needed();
    return *m_pseudo_class_rule_cache[to_underlying(pseudo_class)];
}

InvalidationSet StyleComputer::invalidation_set_for_properties(Vector<InvalidationSet::Property> const& properties) const
{
    if (!m_style_invalidation_data)
        return {};
    auto const& descendant_invalidation_sets = m_style_invalidation_data->descendant_invalidation_sets;
    InvalidationSet result;
    for (auto const& property : properties) {
        if (auto it = descendant_invalidation_sets.find(property); it != descendant_invalidation_sets.end())
            result.include_all_from(it->value);
    }
    return result;
}

bool StyleComputer::invalidation_property_used_in_has_selector(InvalidationSet::Property const& property) const
{
    if (!m_style_invalidation_data)
        return true;
    switch (property.type) {
    case InvalidationSet::Property::Type::Id:
        if (m_style_invalidation_data->ids_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::Class:
        if (m_style_invalidation_data->class_names_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::Attribute:
        if (m_style_invalidation_data->attribute_names_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::TagName:
        if (m_style_invalidation_data->tag_names_used_in_has_selectors.contains(property.name()))
            return true;
        break;
    case InvalidationSet::Property::Type::PseudoClass:
        if (m_style_invalidation_data->pseudo_classes_used_in_has_selectors.contains(property.value.get<PseudoClass>()))
            return true;
        break;
    default:
        break;
    }
    return false;
}

Vector<MatchingRule const*> StyleComputer::collect_matching_rules(DOM::Element const& element, CascadeOrigin cascade_origin, Optional<CSS::PseudoElement> pseudo_element, PseudoClassBitmap& attempted_pseudo_class_matches, Optional<FlyString const> qualified_layer_name) const
{
    auto const& root_node = element.root();
    auto shadow_root = is<DOM::ShadowRoot>(root_node) ? static_cast<DOM::ShadowRoot const*>(&root_node) : nullptr;
    auto element_shadow_root = element.shadow_root();
    auto const& element_namespace_uri = element.namespace_uri();

    GC::Ptr<DOM::Element const> shadow_host;
    if (element_shadow_root)
        shadow_host = element;
    else if (shadow_root)
        shadow_host = shadow_root->host();

    Vector<MatchingRule const&, 512> rules_to_run;

    auto add_rule_to_run = [&](MatchingRule const& rule_to_run) {
        // FIXME: This needs to be revised when adding support for the ::shadow selector, as it needs to cross shadow boundaries.
        auto rule_root = rule_to_run.shadow_root;
        auto from_user_agent_or_user_stylesheet = rule_to_run.cascade_origin == CascadeOrigin::UserAgent || rule_to_run.cascade_origin == CascadeOrigin::User;

        // NOTE: Inside shadow trees, we only match rules that are defined in the shadow tree's style sheets.
        //       The key exception is the shadow tree's *shadow host*, which needs to match :host rules from inside the shadow root.
        //       Also note that UA or User style sheets don't have a scope, so they are always relevant.
        // FIXME: We should reorganize the data so that the document-level StyleComputer doesn't cache *all* rules,
        //        but instead we'd have some kind of "style scope" at the document level, and also one for each shadow root.
        //        Then we could only evaluate rules from the current style scope.
        bool rule_is_relevant_for_current_scope = rule_root == shadow_root
            || (element_shadow_root && rule_root == element_shadow_root)
            || from_user_agent_or_user_stylesheet;

        if (!rule_is_relevant_for_current_scope)
            return;

        auto const& selector = rule_to_run.selector;
        if (selector.can_use_ancestor_filter() && should_reject_with_ancestor_filter(selector))
            return;

        rules_to_run.unchecked_append(rule_to_run);
    };

    auto add_rules_to_run = [&](Vector<MatchingRule> const& rules) {
        rules_to_run.grow_capacity(rules_to_run.size() + rules.size());
        if (pseudo_element.has_value()) {
            for (auto const& rule : rules) {
                if (rule.contains_pseudo_element && filter_namespace_rule(element_namespace_uri, rule))
                    add_rule_to_run(rule);
            }
        } else {
            for (auto const& rule : rules) {
                if (!rule.contains_pseudo_element && filter_namespace_rule(element_namespace_uri, rule))
                    add_rule_to_run(rule);
            }
        }
    };

    auto add_rules_from_cache = [&](RuleCache const& rule_cache) {
        rule_cache.for_each_matching_rules(element, pseudo_element, [&](auto const& matching_rules) {
            add_rules_to_run(matching_rules);
            return IterationDecision::Continue;
        });
    };

    if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, nullptr))
        add_rules_from_cache(*rule_cache);

    if (shadow_root) {
        if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, shadow_root))
            add_rules_from_cache(*rule_cache);
    }

    if (element_shadow_root) {
        if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, element_shadow_root))
            add_rules_from_cache(*rule_cache);
    }

    Vector<MatchingRule const*> matching_rules;
    matching_rules.ensure_capacity(rules_to_run.size());

    for (auto const& rule_to_run : rules_to_run) {
        // NOTE: When matching an element against a rule from outside the shadow root's style scope,
        //       we have to pass in null for the shadow host, otherwise combinator traversal will
        //       be confined to the element itself (since it refuses to cross the shadow boundary).
        auto rule_root = rule_to_run.shadow_root;
        auto shadow_host_to_use = shadow_host;
        if (element.is_shadow_host() && rule_root != element.shadow_root())
            shadow_host_to_use = nullptr;

        auto const& selector = rule_to_run.selector;

        SelectorEngine::MatchContext context {
            .style_sheet_for_rule = *rule_to_run.sheet,
            .subject = element,
            .collect_per_element_selector_involvement_metadata = true,
        };
        ScopeGuard guard = [&] {
            attempted_pseudo_class_matches |= context.attempted_pseudo_class_matches;
        };
        if (!SelectorEngine::matches(selector, element, shadow_host_to_use, context, pseudo_element))
            continue;
        matching_rules.append(&rule_to_run);
    }

    return matching_rules;
}

static void sort_matching_rules(Vector<MatchingRule const*>& matching_rules)
{
    quick_sort(matching_rules, [&](MatchingRule const* a, MatchingRule const* b) {
        auto const& a_selector = a->selector;
        auto const& b_selector = b->selector;
        auto a_specificity = a_selector.specificity();
        auto b_specificity = b_selector.specificity();
        if (a_specificity == b_specificity) {
            if (a->style_sheet_index == b->style_sheet_index)
                return a->rule_index < b->rule_index;
            return a->style_sheet_index < b->style_sheet_index;
        }
        return a_specificity < b_specificity;
    });
}

void StyleComputer::for_each_property_expanding_shorthands(PropertyID property_id, CSSStyleValue const& value, Function<void(PropertyID, CSSStyleValue const&)> const& set_longhand_property)
{
    if (property_is_shorthand(property_id) && (value.is_unresolved() || value.is_pending_substitution())) {
        // If a shorthand property contains an arbitrary substitution function in its value, the longhand properties
        // it’s associated with must instead be filled in with a special, unobservable-to-authors pending-substitution
        // value that indicates the shorthand contains an arbitrary substitution function, and thus the longhand’s
        // value can’t be determined until after substituted.
        // https://drafts.csswg.org/css-values-5/#pending-substitution-value
        // Ensure we keep the longhand around until it can be resolved.
        set_longhand_property(property_id, value);
        auto pending_substitution_value = PendingSubstitutionStyleValue::create();
        for (auto longhand_id : longhands_for_shorthand(property_id)) {
            for_each_property_expanding_shorthands(longhand_id, pending_substitution_value, set_longhand_property);
        }
        return;
    }

    if (value.is_shorthand()) {
        auto& shorthand_value = value.as_shorthand();
        auto& properties = shorthand_value.sub_properties();
        auto& values = shorthand_value.values();
        for (size_t i = 0; i < properties.size(); ++i)
            for_each_property_expanding_shorthands(properties[i], values[i], set_longhand_property);
        return;
    }

    // FIXME: We should add logic in parse_css_value to parse "positional-value-list-shorthand"s as
    //        ShorthandStyleValues to avoid the need for this (and assign_start_and_end_values).
    auto assign_edge_values = [&](PropertyID top_property, PropertyID right_property, PropertyID bottom_property, PropertyID left_property, CSSStyleValue const& value) {
        if (value.is_value_list()) {
            auto values = value.as_value_list().values();

            if (values.size() == 4) {
                set_longhand_property(top_property, values[0]);
                set_longhand_property(right_property, values[1]);
                set_longhand_property(bottom_property, values[2]);
                set_longhand_property(left_property, values[3]);
            } else if (values.size() == 3) {
                set_longhand_property(top_property, values[0]);
                set_longhand_property(right_property, values[1]);
                set_longhand_property(bottom_property, values[2]);
                set_longhand_property(left_property, values[1]);
            } else if (values.size() == 2) {
                set_longhand_property(top_property, values[0]);
                set_longhand_property(right_property, values[1]);
                set_longhand_property(bottom_property, values[0]);
                set_longhand_property(left_property, values[1]);
            } else if (values.size() == 1) {
                set_longhand_property(top_property, values[0]);
                set_longhand_property(right_property, values[0]);
                set_longhand_property(bottom_property, values[0]);
                set_longhand_property(left_property, values[0]);
            }
        } else {
            set_longhand_property(top_property, value);
            set_longhand_property(right_property, value);
            set_longhand_property(bottom_property, value);
            set_longhand_property(left_property, value);
        }
    };

    auto assign_start_and_end_values = [&](PropertyID start_property, PropertyID end_property, auto const& values) {
        if (values.is_value_list()) {
            set_longhand_property(start_property, value.as_value_list().values()[0]);
            set_longhand_property(end_property, value.as_value_list().values()[1]);
        } else {
            set_longhand_property(start_property, value);
            set_longhand_property(end_property, value);
        }
    };

    if (property_id == CSS::PropertyID::BorderStyle) {
        assign_edge_values(PropertyID::BorderTopStyle, PropertyID::BorderRightStyle, PropertyID::BorderBottomStyle, PropertyID::BorderLeftStyle, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderBlockStyle) {
        assign_start_and_end_values(PropertyID::BorderBlockStartStyle, PropertyID::BorderBlockEndStyle, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderInlineStyle) {
        assign_start_and_end_values(PropertyID::BorderInlineStartStyle, PropertyID::BorderInlineEndStyle, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderWidth) {
        assign_edge_values(PropertyID::BorderTopWidth, PropertyID::BorderRightWidth, PropertyID::BorderBottomWidth, PropertyID::BorderLeftWidth, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderBlockWidth) {
        assign_start_and_end_values(PropertyID::BorderBlockStartWidth, PropertyID::BorderBlockEndWidth, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderInlineWidth) {
        assign_start_and_end_values(PropertyID::BorderInlineStartWidth, PropertyID::BorderInlineEndWidth, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderColor) {
        assign_edge_values(PropertyID::BorderTopColor, PropertyID::BorderRightColor, PropertyID::BorderBottomColor, PropertyID::BorderLeftColor, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderBlockColor) {
        assign_start_and_end_values(PropertyID::BorderBlockStartColor, PropertyID::BorderBlockEndColor, value);
        return;
    }

    if (property_id == CSS::PropertyID::BorderInlineColor) {
        assign_start_and_end_values(PropertyID::BorderInlineStartColor, PropertyID::BorderInlineEndColor, value);
        return;
    }

    if (property_id == CSS::PropertyID::BackgroundPosition) {
        if (value.is_position()) {
            auto const& position = value.as_position();
            set_longhand_property(CSS::PropertyID::BackgroundPositionX, position.edge_x());
            set_longhand_property(CSS::PropertyID::BackgroundPositionY, position.edge_y());
        } else if (value.is_value_list()) {
            // Expand background-position layer list into separate lists for x and y positions:
            auto const& values_list = value.as_value_list();
            StyleValueVector x_positions {};
            StyleValueVector y_positions {};
            x_positions.ensure_capacity(values_list.size());
            y_positions.ensure_capacity(values_list.size());
            for (auto& layer : values_list.values()) {
                if (layer->is_position()) {
                    auto const& position = layer->as_position();
                    x_positions.unchecked_append(position.edge_x());
                    y_positions.unchecked_append(position.edge_y());
                } else {
                    x_positions.unchecked_append(layer);
                    y_positions.unchecked_append(layer);
                }
            }
            set_longhand_property(CSS::PropertyID::BackgroundPositionX, StyleValueList::create(move(x_positions), values_list.separator()));
            set_longhand_property(CSS::PropertyID::BackgroundPositionY, StyleValueList::create(move(y_positions), values_list.separator()));
        } else {
            set_longhand_property(CSS::PropertyID::BackgroundPositionX, value);
            set_longhand_property(CSS::PropertyID::BackgroundPositionY, value);
        }

        return;
    }

    if (property_id == CSS::PropertyID::Inset) {
        assign_edge_values(PropertyID::Top, PropertyID::Right, PropertyID::Bottom, PropertyID::Left, value);
        return;
    }

    if (property_id == CSS::PropertyID::InsetBlock) {
        assign_start_and_end_values(PropertyID::InsetBlockStart, PropertyID::InsetBlockEnd, value);
        return;
    }

    if (property_id == CSS::PropertyID::InsetInline) {
        assign_start_and_end_values(PropertyID::InsetInlineStart, PropertyID::InsetInlineEnd, value);
        return;
    }

    if (property_id == CSS::PropertyID::Margin) {
        assign_edge_values(PropertyID::MarginTop, PropertyID::MarginRight, PropertyID::MarginBottom, PropertyID::MarginLeft, value);
        return;
    }

    if (property_id == CSS::PropertyID::MarginBlock) {
        assign_start_and_end_values(PropertyID::MarginBlockStart, PropertyID::MarginBlockEnd, value);
        return;
    }

    if (property_id == CSS::PropertyID::MarginInline) {
        assign_start_and_end_values(PropertyID::MarginInlineStart, PropertyID::MarginInlineEnd, value);
        return;
    }

    if (property_id == CSS::PropertyID::Padding) {
        assign_edge_values(PropertyID::PaddingTop, PropertyID::PaddingRight, PropertyID::PaddingBottom, PropertyID::PaddingLeft, value);
        return;
    }

    if (property_id == CSS::PropertyID::PaddingBlock) {
        assign_start_and_end_values(PropertyID::PaddingBlockStart, PropertyID::PaddingBlockEnd, value);
        return;
    }

    if (property_id == CSS::PropertyID::PaddingInline) {
        assign_start_and_end_values(PropertyID::PaddingInlineStart, PropertyID::PaddingInlineEnd, value);
        return;
    }

    if (property_id == CSS::PropertyID::Gap) {
        if (value.is_value_list()) {
            auto const& values_list = value.as_value_list();
            set_longhand_property(CSS::PropertyID::RowGap, values_list.values()[0]);
            set_longhand_property(CSS::PropertyID::ColumnGap, values_list.values()[1]);
            return;
        }
        set_longhand_property(CSS::PropertyID::RowGap, value);
        set_longhand_property(CSS::PropertyID::ColumnGap, value);
        return;
    }

    if (property_id == CSS::PropertyID::Transition) {
        if (value.to_keyword() == Keyword::None) {
            // Handle `none` as a shorthand for `all 0s ease 0s`.
            set_longhand_property(CSS::PropertyID::TransitionProperty, CSSKeywordValue::create(Keyword::All));
            set_longhand_property(CSS::PropertyID::TransitionDuration, TimeStyleValue::create(CSS::Time::make_seconds(0)));
            set_longhand_property(CSS::PropertyID::TransitionDelay, TimeStyleValue::create(CSS::Time::make_seconds(0)));
            set_longhand_property(CSS::PropertyID::TransitionTimingFunction, EasingStyleValue::create(EasingStyleValue::CubicBezier::ease()));
            set_longhand_property(CSS::PropertyID::TransitionBehavior, CSSKeywordValue::create(Keyword::Normal));
        } else if (value.is_transition()) {
            auto const& transitions = value.as_transition().transitions();
            Array<Vector<ValueComparingNonnullRefPtr<CSSStyleValue const>>, 5> transition_values;
            for (auto const& transition : transitions) {
                transition_values[0].append(*transition.property_name);
                transition_values[1].append(transition.duration.as_style_value());
                transition_values[2].append(transition.delay.as_style_value());
                if (transition.easing)
                    transition_values[3].append(*transition.easing);
                transition_values[4].append(CSSKeywordValue::create(to_keyword(transition.transition_behavior)));
            }

            set_longhand_property(CSS::PropertyID::TransitionProperty, StyleValueList::create(move(transition_values[0]), StyleValueList::Separator::Comma));
            set_longhand_property(CSS::PropertyID::TransitionDuration, StyleValueList::create(move(transition_values[1]), StyleValueList::Separator::Comma));
            set_longhand_property(CSS::PropertyID::TransitionDelay, StyleValueList::create(move(transition_values[2]), StyleValueList::Separator::Comma));
            set_longhand_property(CSS::PropertyID::TransitionTimingFunction, StyleValueList::create(move(transition_values[3]), StyleValueList::Separator::Comma));
            set_longhand_property(CSS::PropertyID::TransitionBehavior, StyleValueList::create(move(transition_values[4]), StyleValueList::Separator::Comma));
        } else {
            set_longhand_property(CSS::PropertyID::TransitionProperty, value);
            set_longhand_property(CSS::PropertyID::TransitionDuration, value);
            set_longhand_property(CSS::PropertyID::TransitionDelay, value);
            set_longhand_property(CSS::PropertyID::TransitionTimingFunction, value);
            set_longhand_property(CSS::PropertyID::TransitionBehavior, value);
        }

        return;
    }

    if (property_is_shorthand(property_id)) {
        // ShorthandStyleValue was handled already, as were unresolved shorthands.
        // That means the only values we should see are the CSS-wide keywords, or the guaranteed-invalid value.
        // Both should be applied to our longhand properties.
        // We don't directly call `set_longhand_property()` because the longhands might have longhands of their own.
        // (eg `grid` -> `grid-template` -> `grid-template-areas` & `grid-template-rows` & `grid-template-columns`)
        VERIFY(value.is_css_wide_keyword() || value.is_guaranteed_invalid());
        for (auto longhand : longhands_for_shorthand(property_id))
            for_each_property_expanding_shorthands(longhand, value, set_longhand_property);
        return;
    }

    set_longhand_property(property_id, value);
}

void StyleComputer::cascade_declarations(
    CascadedProperties& cascaded_properties,
    DOM::Element& element,
    Optional<CSS::PseudoElement> pseudo_element,
    Vector<MatchingRule const*> const& matching_rules,
    CascadeOrigin cascade_origin,
    Important important,
    Optional<FlyString> layer_name,
    Optional<LogicalAliasMappingContext> logical_alias_mapping_context,
    ReadonlySpan<PropertyID> properties_to_cascade) const
{
    auto seen_properties = MUST(Bitmap::create(to_underlying(last_property_id) + 1, false));
    auto cascade_style_declaration = [&](CSSStyleProperties const& declaration) {
        seen_properties.fill(false);
        for (auto const& property : declaration.properties()) {

            // OPTIMIZATION: If we've been asked to only cascade a specific set of properties, skip the rest.
            if (!properties_to_cascade.is_empty()) {
                if (!properties_to_cascade.contains_slow(property.property_id))
                    continue;
            }

            if (important != property.important)
                continue;

            if (pseudo_element.has_value() && !pseudo_element_supports_property(*pseudo_element, property.property_id))
                continue;

            auto property_value = property.value;

            if (property_value->is_unresolved())
                property_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { element.document() }, element, pseudo_element, property.property_id, property_value->as_unresolved());

            if (property_value->is_guaranteed_invalid()) {
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                // When substitution results in a property’s value containing the guaranteed-invalid value, this makes the
                // declaration invalid at computed-value time. When this happens, the computed value is one of the
                // following depending on the property’s type:

                // -> The property is a non-registered custom property
                // -> The property is a registered custom property with universal syntax
                // FIXME: Process custom properties here?
                if (false) {
                    // The computed value is the guaranteed-invalid value.
                }
                // -> Otherwise
                else {
                    // Either the property’s inherited value or its initial value depending on whether the property is
                    // inherited or not, respectively, as if the property’s value had been specified as the unset keyword.
                    property_value = CSSKeywordValue::create(Keyword::Unset);
                }
            }

            for_each_property_expanding_shorthands(property.property_id, property_value, [&](PropertyID longhand_id, CSSStyleValue const& longhand_value) {
                // If we're a PSV that's already been seen, that should mean that our shorthand already got
                // resolved and gave us a value, so we don't want to overwrite it with a PSV.
                if (seen_properties.get(to_underlying(longhand_id)) && property_value->is_pending_substitution())
                    return;
                seen_properties.set(to_underlying(longhand_id), true);

                PropertyID physical_property_id;

                if (property_is_logical_alias(longhand_id)) {
                    if (!logical_alias_mapping_context.has_value())
                        return;
                    physical_property_id = map_logical_alias_to_physical_property(longhand_id, logical_alias_mapping_context.value());
                } else {
                    physical_property_id = longhand_id;
                }

                if (longhand_value.is_revert()) {
                    cascaded_properties.revert_property(physical_property_id, important, cascade_origin);
                } else if (longhand_value.is_revert_layer()) {
                    cascaded_properties.revert_layer_property(physical_property_id, important, layer_name);
                } else {
                    cascaded_properties.set_property(physical_property_id, longhand_value, important, cascade_origin, layer_name, declaration);
                }
            });
        }
    };

    for (auto const& match : matching_rules) {
        cascade_style_declaration(match->declaration());
    }

    if (cascade_origin == CascadeOrigin::Author && !pseudo_element.has_value()) {
        if (auto const inline_style = element.inline_style()) {
            cascade_style_declaration(*inline_style);
        }
    }
}

static void cascade_custom_properties(DOM::Element& element, Optional<CSS::PseudoElement> pseudo_element, Vector<MatchingRule const*> const& matching_rules, HashMap<FlyString, StyleProperty>& custom_properties)
{
    size_t needed_capacity = 0;
    for (auto const& matching_rule : matching_rules)
        needed_capacity += matching_rule->declaration().custom_properties().size();

    if (!pseudo_element.has_value()) {
        if (auto const inline_style = element.inline_style())
            needed_capacity += inline_style->custom_properties().size();
    }

    custom_properties.ensure_capacity(custom_properties.size() + needed_capacity);

    for (auto const& matching_rule : matching_rules) {
        for (auto const& it : matching_rule->declaration().custom_properties()) {
            auto style_value = it.value.value;
            if (style_value->is_revert_layer())
                continue;
            custom_properties.set(it.key, it.value);
        }
    }

    if (!pseudo_element.has_value()) {
        if (auto const inline_style = element.inline_style())
            custom_properties.update(inline_style->custom_properties());
    }
}

void StyleComputer::collect_animation_into(DOM::Element& element, Optional<CSS::PseudoElement> pseudo_element, GC::Ref<Animations::KeyframeEffect> effect, ComputedProperties& computed_properties, AnimationRefresh refresh) const
{
    auto animation = effect->associated_animation();
    if (!animation)
        return;

    auto output_progress = effect->transformed_progress();
    if (!output_progress.has_value())
        return;

    if (!effect->key_frame_set())
        return;

    auto& keyframes = effect->key_frame_set()->keyframes_by_key;
    if (keyframes.size() < 2) {
        if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
            dbgln("    Did not find enough keyframes ({} keyframes)", keyframes.size());
            for (auto it = keyframes.begin(); it != keyframes.end(); ++it)
                dbgln("        - {}", it.key());
        }
        return;
    }

    auto key = static_cast<i64>(round(output_progress.value() * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor));
    auto keyframe_start_it = [&] {
        if (output_progress.value() <= 0) {
            return keyframes.begin();
        }
        auto potential_match = keyframes.find_largest_not_above_iterator(key);
        auto next = potential_match;
        ++next;
        if (next.is_end()) {
            --potential_match;
        }
        return potential_match;
    }();
    auto keyframe_start = static_cast<i64>(keyframe_start_it.key());
    auto keyframe_values = *keyframe_start_it;

    auto keyframe_end_it = ++keyframe_start_it;
    VERIFY(!keyframe_end_it.is_end());
    auto keyframe_end = static_cast<i64>(keyframe_end_it.key());
    auto keyframe_end_values = *keyframe_end_it;

    auto progress_in_keyframe
        = static_cast<float>(key - keyframe_start) / static_cast<float>(keyframe_end - keyframe_start);

    if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
        auto valid_properties = keyframe_values.properties.size();
        dbgln("Animation {} contains {} properties to interpolate, progress = {}%", animation->id(), valid_properties, progress_in_keyframe * 100);
    }

    // FIXME: Follow https://drafts.csswg.org/web-animations-1/#ref-for-computed-keyframes in whatever the right place is.
    auto compute_keyframe_values = [refresh, &computed_properties, &element, &pseudo_element, this](auto const& keyframe_values) {
        HashMap<PropertyID, RefPtr<CSSStyleValue const>> result;
        HashMap<PropertyID, PropertyID> longhands_set_by_property_id;
        auto property_is_set_by_use_initial = MUST(Bitmap::create(to_underlying(last_longhand_property_id) - to_underlying(first_longhand_property_id) + 1, false));

        auto property_is_logical_alias_including_shorthands = [&](PropertyID property_id) {
            if (property_is_shorthand(property_id))
                // NOTE: All expanded longhands for a logical alias shorthand are logical aliases so we only need to check the first one.
                return property_is_logical_alias(expanded_longhands_for_shorthand(property_id)[0]);

            return property_is_logical_alias(property_id);
        };

        // https://drafts.csswg.org/web-animations-1/#ref-for-computed-keyframes
        auto is_property_preferred = [&](PropertyID a, PropertyID b) {
            // If conflicts arise when expanding shorthand properties or replacing logical properties with physical properties, apply the following rules in order until the conflict is resolved:
            // 1. Longhand properties override shorthand properties (e.g. border-top-color overrides border-top).
            if (property_is_shorthand(a) != property_is_shorthand(b))
                return !property_is_shorthand(a);

            // 2. Shorthand properties with fewer longhand components override those with more longhand components (e.g. border-top overrides border-color).
            if (property_is_shorthand(a)) {
                auto number_of_expanded_shorthands_a = expanded_longhands_for_shorthand(a).size();
                auto number_of_expanded_shorthands_b = expanded_longhands_for_shorthand(b).size();

                if (number_of_expanded_shorthands_a != number_of_expanded_shorthands_b)
                    return number_of_expanded_shorthands_a < number_of_expanded_shorthands_b;
            }

            auto property_a_is_logical_alias = property_is_logical_alias_including_shorthands(a);
            auto property_b_is_logical_alias = property_is_logical_alias_including_shorthands(b);

            // 3. Physical properties override logical properties.
            if (property_a_is_logical_alias != property_b_is_logical_alias)
                return !property_a_is_logical_alias;

            // 4. For shorthand properties with an equal number of longhand components, properties whose IDL name (see
            //    the CSS property to IDL attribute algorithm [CSSOM]) appears earlier when sorted in ascending order
            //    by the Unicode codepoints that make up each IDL name, override those who appear later.
            return camel_case_string_from_property_id(a) < camel_case_string_from_property_id(b);
        };

        compute_font(computed_properties, &element, pseudo_element);
        Length::FontMetrics font_metrics {
            root_element_font_metrics_for_element(element).font_size,
            computed_properties.first_available_computed_font().pixel_metrics()
        };
        for (auto const& [property_id, value] : keyframe_values.properties) {
            bool is_use_initial = false;

            auto style_value = value.visit(
                [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) -> RefPtr<CSSStyleValue const> {
                    if (refresh == AnimationRefresh::Yes)
                        return {};
                    if (property_is_shorthand(property_id))
                        return {};
                    is_use_initial = true;
                    return computed_properties.property(property_id);
                },
                [&](RefPtr<CSSStyleValue const> value) -> RefPtr<CSSStyleValue const> {
                    return value;
                });

            if (!style_value) {
                result.set(property_id, nullptr);
                continue;
            }

            // If the style value is a PendingSubstitutionStyleValue we should skip it to avoid overwriting any value
            // already set by resolving the relevant shorthand's value.
            if (style_value->is_pending_substitution())
                continue;

            if (style_value->is_revert() || style_value->is_revert_layer())
                style_value = computed_properties.property(property_id);
            if (style_value->is_unresolved())
                style_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { element.document() }, element, pseudo_element, property_id, style_value->as_unresolved());

            for_each_property_expanding_shorthands(property_id, *style_value, [&](PropertyID longhand_id, CSSStyleValue const& longhand_value) {
                auto physical_longhand_id = map_logical_alias_to_physical_property(longhand_id, LogicalAliasMappingContext { computed_properties.writing_mode(), computed_properties.direction() });
                auto physical_longhand_id_bitmap_index = to_underlying(physical_longhand_id) - to_underlying(first_longhand_property_id);

                // Don't overwrite values if this is the result of a UseInitial
                if (result.contains(physical_longhand_id) && result.get(physical_longhand_id) != nullptr && is_use_initial)
                    return;

                // Don't overwrite unless the value was originally set by a UseInitial or this property is preferred over the one that set it originally
                if (result.contains(physical_longhand_id) && result.get(physical_longhand_id) != nullptr && !property_is_set_by_use_initial.get(physical_longhand_id_bitmap_index) && !is_property_preferred(property_id, longhands_set_by_property_id.get(physical_longhand_id).value()))
                    return;

                longhands_set_by_property_id.set(physical_longhand_id, property_id);
                property_is_set_by_use_initial.set(physical_longhand_id_bitmap_index, is_use_initial);
                result.set(physical_longhand_id, { longhand_value.absolutized(viewport_rect(), font_metrics, m_root_element_font_metrics) });
            });
        }
        return result;
    };
    HashMap<PropertyID, RefPtr<CSSStyleValue const>> computed_start_values = compute_keyframe_values(keyframe_values);
    HashMap<PropertyID, RefPtr<CSSStyleValue const>> computed_end_values = compute_keyframe_values(keyframe_end_values);

    for (auto const& it : computed_start_values) {
        auto resolved_start_property = it.value;
        RefPtr resolved_end_property = computed_end_values.get(it.key).value_or(nullptr);

        if (!resolved_end_property) {
            if (resolved_start_property) {
                computed_properties.set_animated_property(it.key, *resolved_start_property);
                dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "No end property for property {}, using {}", string_from_property_id(it.key), resolved_start_property->to_string(SerializationMode::Normal));
            }
            continue;
        }

        if (resolved_end_property && !resolved_start_property)
            resolved_start_property = property_initial_value(it.key);

        if (!resolved_start_property || !resolved_end_property)
            continue;

        auto start = resolved_start_property.release_nonnull();
        auto end = resolved_end_property.release_nonnull();

        if (computed_properties.is_property_important(it.key)) {
            continue;
        }

        if (auto next_value = interpolate_property(*effect->target(), it.key, *start, *end, progress_in_keyframe, AllowDiscrete::Yes)) {
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} = {}", string_from_property_id(it.key), progress_in_keyframe, start->to_string(SerializationMode::Normal), end->to_string(SerializationMode::Normal), next_value->to_string(SerializationMode::Normal));
            computed_properties.set_animated_property(it.key, *next_value);
        } else {
            // If interpolate_property() fails, the element should not be rendered
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} is invalid", string_from_property_id(it.key), progress_in_keyframe, start->to_string(SerializationMode::Normal), end->to_string(SerializationMode::Normal));
            computed_properties.set_animated_property(PropertyID::Visibility, CSSKeywordValue::create(Keyword::Hidden));
        }
    }
}

static void apply_animation_properties(DOM::Document& document, CascadedProperties& cascaded_properties, Animations::Animation& animation)
{
    if (!animation.effect())
        return;

    auto& effect = as<Animations::KeyframeEffect>(*animation.effect());

    Optional<CSS::Time> duration;
    if (auto duration_value = cascaded_properties.property(PropertyID::AnimationDuration); duration_value) {
        if (duration_value->is_time()) {
            duration = duration_value->as_time().time();
        } else if (duration_value->is_keyword() && duration_value->as_keyword().keyword() == Keyword::Auto) {
            // We use empty optional to represent "auto".
            duration = {};
        } else if (duration_value->is_calculated() && duration_value->as_calculated().resolves_to_time()) {
            auto resolved_time = duration_value->as_calculated().resolve_time({});
            if (resolved_time.has_value()) {
                duration = resolved_time.value();
            }
        }
    }

    CSS::Time delay { 0, CSS::Time::Type::S };
    if (auto delay_value = cascaded_properties.property(PropertyID::AnimationDelay); delay_value) {
        if (delay_value->is_time()) {
            delay = delay_value->as_time().time();
        } else if (delay_value->is_calculated() && delay_value->as_calculated().resolves_to_time()) {
            auto resolved_time = delay_value->as_calculated().resolve_time({});
            if (resolved_time.has_value()) {
                delay = resolved_time.value();
            }
        }
    }

    double iteration_count = 1.0;
    if (auto iteration_count_value = cascaded_properties.property(PropertyID::AnimationIterationCount); iteration_count_value) {
        if (iteration_count_value->is_keyword() && iteration_count_value->to_keyword() == Keyword::Infinite)
            iteration_count = HUGE_VAL;
        else if (iteration_count_value->is_number())
            iteration_count = iteration_count_value->as_number().number();
        else if (iteration_count_value->is_calculated() && iteration_count_value->as_calculated().resolves_to_number()) {
            auto resolved_number = iteration_count_value->as_calculated().resolve_number({});
            if (resolved_number.has_value()) {
                iteration_count = resolved_number.value();
            }
        }
    }

    CSS::AnimationFillMode fill_mode { CSS::AnimationFillMode::None };
    if (auto fill_mode_property = cascaded_properties.property(PropertyID::AnimationFillMode); fill_mode_property && fill_mode_property->is_keyword()) {
        if (auto fill_mode_value = keyword_to_animation_fill_mode(fill_mode_property->to_keyword()); fill_mode_value.has_value())
            fill_mode = *fill_mode_value;
    }

    CSS::AnimationDirection direction { CSS::AnimationDirection::Normal };
    if (auto direction_property = cascaded_properties.property(PropertyID::AnimationDirection); direction_property && direction_property->is_keyword()) {
        if (auto direction_value = keyword_to_animation_direction(direction_property->to_keyword()); direction_value.has_value())
            direction = *direction_value;
    }

    CSS::AnimationPlayState play_state { CSS::AnimationPlayState::Running };
    if (auto play_state_property = cascaded_properties.property(PropertyID::AnimationPlayState); play_state_property && play_state_property->is_keyword()) {
        if (auto play_state_value = keyword_to_animation_play_state(play_state_property->to_keyword()); play_state_value.has_value())
            play_state = *play_state_value;
    }

    CSS::EasingStyleValue::Function timing_function { CSS::EasingStyleValue::CubicBezier::ease() };
    if (auto timing_property = cascaded_properties.property(PropertyID::AnimationTimingFunction); timing_property && timing_property->is_easing())
        timing_function = timing_property->as_easing().function();

    auto iteration_duration = duration.has_value()
        ? Variant<double, String> { duration.release_value().to_milliseconds() }
        : "auto"_string;
    effect.set_iteration_duration(iteration_duration);
    effect.set_start_delay(delay.to_milliseconds());
    effect.set_iteration_count(iteration_count);
    effect.set_timing_function(move(timing_function));
    effect.set_fill_mode(Animations::css_fill_mode_to_bindings_fill_mode(fill_mode));
    effect.set_playback_direction(Animations::css_animation_direction_to_bindings_playback_direction(direction));

    if (play_state != effect.last_css_animation_play_state()) {
        if (play_state == CSS::AnimationPlayState::Running && animation.play_state() != Bindings::AnimationPlayState::Running) {
            HTML::TemporaryExecutionContext context(document.realm());
            animation.play().release_value_but_fixme_should_propagate_errors();
        } else if (play_state == CSS::AnimationPlayState::Paused && animation.play_state() != Bindings::AnimationPlayState::Paused) {
            HTML::TemporaryExecutionContext context(document.realm());
            animation.pause().release_value_but_fixme_should_propagate_errors();
        }

        effect.set_last_css_animation_play_state(play_state);
    }
}

static void apply_dimension_attribute(CascadedProperties& cascaded_properties, DOM::Element const& element, FlyString const& attribute_name, CSS::PropertyID property_id)
{
    auto attribute = element.attribute(attribute_name);
    if (!attribute.has_value())
        return;

    auto parsed_value = HTML::parse_dimension_value(*attribute);
    if (!parsed_value)
        return;

    cascaded_properties.set_property_from_presentational_hint(property_id, parsed_value.release_nonnull());
}

static void compute_transitioned_properties(ComputedProperties const& style, DOM::Element& element, Optional<PseudoElement> pseudo_element)
{
    auto const source_declaration = style.transition_property_source();
    if (!source_declaration)
        return;
    if (!element.computed_properties())
        return;
    if (source_declaration == element.cached_transition_property_source(pseudo_element))
        return;
    // Reparse this transition property
    element.clear_transitions(pseudo_element);
    element.set_cached_transition_property_source(pseudo_element, *source_declaration);

    auto const& transition_properties_value = style.property(PropertyID::TransitionProperty);
    auto transition_properties = transition_properties_value.is_value_list()
        ? transition_properties_value.as_value_list().values()
        : StyleValueVector { transition_properties_value };

    Vector<Vector<PropertyID>> properties;

    for (size_t i = 0; i < transition_properties.size(); i++) {
        auto property_value = transition_properties[i];
        Vector<PropertyID> properties_for_this_transition;

        if (property_value->is_keyword()) {
            auto keyword = property_value->as_keyword().keyword();
            if (keyword == Keyword::None)
                continue;
            if (keyword == Keyword::All) {
                for (auto prop = first_property_id; prop != last_property_id; prop = static_cast<PropertyID>(to_underlying(prop) + 1))
                    properties_for_this_transition.append(prop);
            }
        } else {
            auto maybe_property = property_id_from_string(property_value->as_custom_ident().custom_ident());
            if (!maybe_property.has_value())
                continue;

            auto transition_property = maybe_property.release_value();
            if (property_is_shorthand(transition_property)) {
                for (auto const& prop : longhands_for_shorthand(transition_property))
                    properties_for_this_transition.append(prop);
            } else {
                properties_for_this_transition.append(transition_property);
            }
        }

        properties.append(move(properties_for_this_transition));
    }

    auto normalize_transition_length_list = [&properties, &style](PropertyID property, auto make_default_value) {
        auto const* style_value = style.maybe_null_property(property);
        StyleValueVector list;

        if (style_value && !style_value->is_value_list()) {
            for (size_t i = 0; i < properties.size(); i++)
                list.append(*style_value);
            return list;
        }

        if (!style_value || !style_value->is_value_list() || style_value->as_value_list().size() == 0) {
            auto default_value = make_default_value();
            for (size_t i = 0; i < properties.size(); i++)
                list.append(default_value);
            return list;
        }

        auto const& value_list = style_value->as_value_list();
        for (size_t i = 0; i < properties.size(); i++)
            list.append(value_list.value_at(i, true));

        return list;
    };

    auto delays = normalize_transition_length_list(
        PropertyID::TransitionDelay,
        [] { return TimeStyleValue::create(Time::make_seconds(0.0)); });
    auto durations = normalize_transition_length_list(
        PropertyID::TransitionDuration,
        [] { return TimeStyleValue::create(Time::make_seconds(0.0)); });
    auto timing_functions = normalize_transition_length_list(
        PropertyID::TransitionTimingFunction,
        [] { return EasingStyleValue::create(EasingStyleValue::CubicBezier::ease()); });
    auto transition_behaviors = normalize_transition_length_list(
        PropertyID::TransitionBehavior,
        [] { return CSSKeywordValue::create(Keyword::None); });

    element.add_transitioned_properties(pseudo_element, move(properties), move(delays), move(durations), move(timing_functions), move(transition_behaviors));
}

// https://drafts.csswg.org/css-transitions/#starting
void StyleComputer::start_needed_transitions(ComputedProperties const& previous_style, ComputedProperties& new_style, DOM::Element& element, Optional<PseudoElement> pseudo_element) const
{

    // https://drafts.csswg.org/css-transitions/#transition-combined-duration
    auto combined_duration = [](Animations::Animatable::TransitionAttributes const& transition_attributes) {
        // Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay.
        return max(transition_attributes.duration, 0) + transition_attributes.delay;
    };

    // For each element and property, the implementation must act as follows:
    auto style_change_event_time = m_document->timeline()->current_time().value();

    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto matching_transition_properties = element.property_transition_attributes(pseudo_element, property_id);
        auto const& before_change_value = previous_style.property(property_id, ComputedProperties::WithAnimationsApplied::Yes);
        auto const& after_change_value = new_style.property(property_id, ComputedProperties::WithAnimationsApplied::No);

        auto existing_transition = element.property_transition(pseudo_element, property_id);
        bool has_running_transition = existing_transition && !existing_transition->is_finished();
        bool has_completed_transition = existing_transition && existing_transition->is_finished();

        auto start_a_transition = [&](auto start_time, auto end_time, auto const& start_value, auto const& end_value, auto const& reversing_adjusted_start_value, auto reversing_shortening_factor) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Starting a transition of {} from {} to {}", string_from_property_id(property_id), start_value->to_string(), end_value->to_string());

            auto transition = CSSTransition::start_a_transition(element, pseudo_element, property_id,
                document().transition_generation(), start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            // Immediately set the property's value to the transition's current value, to prevent single-frame jumps.
            collect_animation_into(element, {}, as<Animations::KeyframeEffect>(*transition->effect()), new_style, AnimationRefresh::No);
        };

        // 1. If all of the following are true:
        if (
            // - the element does not have a running transition for the property,
            (!has_running_transition) &&
            // - there is a matching transition-property value, and
            (matching_transition_properties.has_value()) &&
            // - the before-change style is different from the after-change style for that property, and the values for the property are transitionable,
            (!before_change_value.equals(after_change_value) && property_values_are_transitionable(property_id, before_change_value, after_change_value, element, matching_transition_properties->transition_behavior)) &&
            // - the element does not have a completed transition for the property
            //   or the end value of the completed transition is different from the after-change style for the property,
            (!has_completed_transition || !existing_transition->transition_end_value()->equals(after_change_value)) &&
            // - the combined duration is greater than 0s,
            (combined_duration(matching_transition_properties.value()) > 0)) {

            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 1.");

            // then implementations must remove the completed transition (if present) from the set of completed transitions
            if (has_completed_transition)
                element.remove_transition(pseudo_element, property_id);
            // and start a transition whose:

            // - start time is the time of the style change event plus the matching transition delay,
            auto start_time = style_change_event_time + matching_transition_properties->delay;

            // - end time is the start time plus the matching transition duration,
            auto end_time = start_time + matching_transition_properties->duration;

            // - start value is the value of the transitioning property in the before-change style,
            auto const& start_value = before_change_value;

            // - end value is the value of the transitioning property in the after-change style,
            auto const& end_value = after_change_value;

            // - reversing-adjusted start value is the same as the start value, and
            auto const& reversing_adjusted_start_value = start_value;

            // - reversing shortening factor is 1.
            double reversing_shortening_factor = 1;

            start_a_transition(start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
        }

        // 2. Otherwise, if the element has a completed transition for the property
        //    and the end value of the completed transition is different from the after-change style for the property,
        //    then implementations must remove the completed transition from the set of completed transitions.
        else if (has_completed_transition && !existing_transition->transition_end_value()->equals(after_change_value)) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 2.");
            element.remove_transition(pseudo_element, property_id);
        }

        // 3. If the element has a running transition or completed transition for the property,
        //    and there is not a matching transition-property value,
        if (existing_transition && !matching_transition_properties.has_value()) {
            // then implementations must cancel the running transition or remove the completed transition from the set of completed transitions.
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 3.");
            if (has_running_transition)
                existing_transition->cancel();
            else
                element.remove_transition(pseudo_element, property_id);
        }

        // 4. If the element has a running transition for the property,
        //    there is a matching transition-property value,
        //    and the end value of the running transition is not equal to the value of the property in the after-change style, then:
        if (has_running_transition && matching_transition_properties.has_value() && !existing_transition->transition_end_value()->equals(after_change_value)) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4. existing end value = {}, after change value = {}", existing_transition->transition_end_value()->to_string(SerializationMode::Normal), after_change_value.to_string(SerializationMode::Normal));
            // 1. If the current value of the property in the running transition is equal to the value of the property in the after-change style,
            //    or if these two values are not transitionable,
            //    then implementations must cancel the running transition.
            auto& current_value = new_style.property(property_id, ComputedProperties::WithAnimationsApplied::Yes);
            if (current_value.equals(after_change_value) || !property_values_are_transitionable(property_id, current_value, after_change_value, element, matching_transition_properties->transition_behavior)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.1");
                existing_transition->cancel();
            }

            // 2. Otherwise, if the combined duration is less than or equal to 0s,
            //    or if the current value of the property in the running transition is not transitionable with the value of the property in the after-change style,
            //    then implementations must cancel the running transition.
            else if ((combined_duration(matching_transition_properties.value()) <= 0)
                || !property_values_are_transitionable(property_id, current_value, after_change_value, element, matching_transition_properties->transition_behavior)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.2");
                existing_transition->cancel();
            }

            // 3. Otherwise, if the reversing-adjusted start value of the running transition is the same as the value of the property in the after-change style
            //    (see the section on reversing of transitions for why these case exists),
            else if (existing_transition->reversing_adjusted_start_value()->equals(after_change_value)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.3");
                // implementations must cancel the running transition and start a new transition whose:
                existing_transition->cancel();
                // AD-HOC: Remove the cancelled transition, otherwise it breaks the invariant that there is only one
                // running or completed transition for a property at once.
                element.remove_transition(pseudo_element, property_id);

                // - reversing-adjusted start value is the end value of the running transition,
                auto reversing_adjusted_start_value = existing_transition->transition_end_value();

                // - reversing shortening factor is the absolute value, clamped to the range [0, 1], of the sum of:
                //   1. the output of the timing function of the old transition at the time of the style change event,
                //      times the reversing shortening factor of the old transition
                auto term_1 = existing_transition->timing_function_output_at_time(style_change_event_time) * existing_transition->reversing_shortening_factor();
                //   2. 1 minus the reversing shortening factor of the old transition.
                auto term_2 = 1 - existing_transition->reversing_shortening_factor();
                double reversing_shortening_factor = clamp(abs(term_1 + term_2), 0.0, 1.0);

                // - start time is the time of the style change event plus:
                //   1. if the matching transition delay is nonnegative, the matching transition delay, or
                //   2. if the matching transition delay is negative, the product of the new transition’s reversing shortening factor and the matching transition delay,
                auto start_time = style_change_event_time
                    + (matching_transition_properties->delay >= 0
                            ? (matching_transition_properties->delay)
                            : (reversing_shortening_factor * matching_transition_properties->delay));

                // - end time is the start time plus the product of the matching transition duration and the new transition’s reversing shortening factor,
                auto end_time = start_time + (matching_transition_properties->duration * reversing_shortening_factor);

                // - start value is the current value of the property in the running transition,
                auto const& start_value = current_value;

                // - end value is the value of the property in the after-change style,
                auto const& end_value = after_change_value;

                start_a_transition(start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            }

            // 4. Otherwise,
            else {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.4");
                // implementations must cancel the running transition and start a new transition whose:
                existing_transition->cancel();
                // AD-HOC: Remove the cancelled transition, otherwise it breaks the invariant that there is only one
                // running or completed transition for a property at once.
                element.remove_transition(pseudo_element, property_id);

                // - start time is the time of the style change event plus the matching transition delay,
                auto start_time = style_change_event_time + matching_transition_properties->delay;

                // - end time is the start time plus the matching transition duration,
                auto end_time = start_time + matching_transition_properties->duration;

                // - start value is the current value of the property in the running transition,
                auto const& start_value = current_value;

                // - end value is the value of the property in the after-change style,
                auto const& end_value = after_change_value;

                // - reversing-adjusted start value is the same as the start value, and
                auto const& reversing_adjusted_start_value = start_value;

                // - reversing shortening factor is 1.
                double reversing_shortening_factor = 1;

                start_a_transition(start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            }
        }
    }
}

StyleComputer::MatchingRuleSet StyleComputer::build_matching_rule_set(DOM::Element const& element, Optional<PseudoElement> pseudo_element, PseudoClassBitmap& attempted_pseudo_class_matches, bool& did_match_any_pseudo_element_rules, ComputeStyleMode mode) const
{
    // First, we collect all the CSS rules whose selectors match `element`:
    MatchingRuleSet matching_rule_set;
    matching_rule_set.user_agent_rules = collect_matching_rules(element, CascadeOrigin::UserAgent, pseudo_element, attempted_pseudo_class_matches);
    sort_matching_rules(matching_rule_set.user_agent_rules);
    matching_rule_set.user_rules = collect_matching_rules(element, CascadeOrigin::User, pseudo_element, attempted_pseudo_class_matches);
    sort_matching_rules(matching_rule_set.user_rules);

    // @layer-ed author rules
    for (auto const& layer_name : m_qualified_layer_names_in_order) {
        auto layer_rules = collect_matching_rules(element, CascadeOrigin::Author, pseudo_element, attempted_pseudo_class_matches, layer_name);
        sort_matching_rules(layer_rules);
        matching_rule_set.author_rules.append({ layer_name, layer_rules });
    }
    // Un-@layer-ed author rules
    auto unlayered_author_rules = collect_matching_rules(element, CascadeOrigin::Author, pseudo_element, attempted_pseudo_class_matches);
    sort_matching_rules(unlayered_author_rules);
    matching_rule_set.author_rules.append({ {}, unlayered_author_rules });

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        VERIFY(pseudo_element.has_value());
        did_match_any_pseudo_element_rules = !matching_rule_set.author_rules.is_empty()
            || !matching_rule_set.user_rules.is_empty()
            || !matching_rule_set.user_agent_rules.is_empty();
    }
    return matching_rule_set;
}

// https://www.w3.org/TR/css-cascade/#cascading
// https://drafts.csswg.org/css-cascade-5/#layering
GC::Ref<CascadedProperties> StyleComputer::compute_cascaded_values(DOM::Element& element, Optional<CSS::PseudoElement> pseudo_element, bool did_match_any_pseudo_element_rules, ComputeStyleMode mode, MatchingRuleSet const& matching_rule_set, Optional<LogicalAliasMappingContext> logical_alias_mapping_context, ReadonlySpan<PropertyID> properties_to_cascade) const
{
    auto cascaded_properties = m_document->heap().allocate<CascadedProperties>();
    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        if (!did_match_any_pseudo_element_rules)
            return cascaded_properties;
    }

    // Normal user agent declarations
    cascade_declarations(*cascaded_properties, element, pseudo_element, matching_rule_set.user_agent_rules, CascadeOrigin::UserAgent, Important::No, {}, logical_alias_mapping_context, properties_to_cascade);

    // Normal user declarations
    cascade_declarations(*cascaded_properties, element, pseudo_element, matching_rule_set.user_rules, CascadeOrigin::User, Important::No, {}, logical_alias_mapping_context, properties_to_cascade);

    // Author presentational hints
    // The spec calls this a special "Author presentational hint origin":
    // "For the purpose of cascading this author presentational hint origin is treated as an independent origin;
    // however for the purpose of the revert keyword (but not for the revert-layer keyword) it is considered
    // part of the author origin."
    // https://drafts.csswg.org/css-cascade-5/#author-presentational-hint-origin
    if (!pseudo_element.has_value()) {
        element.apply_presentational_hints(cascaded_properties);
        if (element.supports_dimension_attributes()) {
            apply_dimension_attribute(cascaded_properties, element, HTML::AttributeNames::width, CSS::PropertyID::Width);
            apply_dimension_attribute(cascaded_properties, element, HTML::AttributeNames::height, CSS::PropertyID::Height);
        }

        // SVG presentation attributes are parsed as CSS values, so we need to handle potential custom properties here.
        if (element.is_svg_element()) {
            cascaded_properties->resolve_unresolved_properties(element, pseudo_element);
        }
    }

    // Normal author declarations, ordered by @layer, with un-@layer-ed rules last
    for (auto const& layer : matching_rule_set.author_rules) {
        cascade_declarations(cascaded_properties, element, pseudo_element, layer.rules, CascadeOrigin::Author, Important::No, layer.qualified_layer_name, logical_alias_mapping_context, properties_to_cascade);
    }

    // Important author declarations, with un-@layer-ed rules first, followed by each @layer in reverse order.
    for (auto const& layer : matching_rule_set.author_rules.in_reverse()) {
        cascade_declarations(cascaded_properties, element, pseudo_element, layer.rules, CascadeOrigin::Author, Important::Yes, {}, logical_alias_mapping_context, properties_to_cascade);
    }

    // Important user declarations
    cascade_declarations(cascaded_properties, element, pseudo_element, matching_rule_set.user_rules, CascadeOrigin::User, Important::Yes, {}, logical_alias_mapping_context, properties_to_cascade);

    // Important user agent declarations
    cascade_declarations(cascaded_properties, element, pseudo_element, matching_rule_set.user_agent_rules, CascadeOrigin::UserAgent, Important::Yes, {}, logical_alias_mapping_context, properties_to_cascade);

    // Transition declarations [css-transitions-1]
    // Note that we have to do these after finishing computing the style,
    // so they're not done here, but as the final step in compute_properties()

    return cascaded_properties;
}

DOM::Element const* element_to_inherit_style_from(DOM::Element const* element, Optional<CSS::PseudoElement> pseudo_element)
{
    // Pseudo-elements treat their originating element as their parent.
    DOM::Element const* parent_element = nullptr;
    if (pseudo_element.has_value()) {
        parent_element = element;
    } else if (element) {
        parent_element = element->parent_or_shadow_host_element();
    }
    return parent_element;
}

NonnullRefPtr<CSSStyleValue const> StyleComputer::get_inherit_value(CSS::PropertyID property_id, DOM::Element const* element, Optional<CSS::PseudoElement> pseudo_element)
{
    auto* parent_element = element_to_inherit_style_from(element, pseudo_element);

    if (!parent_element || !parent_element->computed_properties())
        return property_initial_value(property_id);
    return parent_element->computed_properties()->property(property_id);
}

void StyleComputer::compute_defaulted_property_value(ComputedProperties& style, DOM::Element const* element, CSS::PropertyID property_id, Optional<CSS::PseudoElement> pseudo_element) const
{
    auto& value_slot = style.m_property_values[to_underlying(property_id)];
    if (!value_slot) {
        if (is_inherited_property(property_id)) {
            style.set_property(
                property_id,
                get_inherit_value(property_id, element, pseudo_element),
                ComputedProperties::Inherited::Yes,
                Important::No);
        } else {
            style.set_property(property_id, property_initial_value(property_id));
        }
        return;
    }

    if (value_slot->is_initial()) {
        value_slot = property_initial_value(property_id);
        return;
    }

    if (value_slot->is_inherit()) {
        value_slot = get_inherit_value(property_id, element, pseudo_element);
        style.set_property_inherited(property_id, ComputedProperties::Inherited::Yes);
        return;
    }

    // https://www.w3.org/TR/css-cascade-4/#inherit-initial
    // If the cascaded value of a property is the unset keyword,
    if (value_slot->is_unset()) {
        if (is_inherited_property(property_id)) {
            // then if it is an inherited property, this is treated as inherit,
            value_slot = get_inherit_value(property_id, element, pseudo_element);
            style.set_property_inherited(property_id, ComputedProperties::Inherited::Yes);
        } else {
            // and if it is not, this is treated as initial.
            value_slot = property_initial_value(property_id);
        }
    }
}

// https://www.w3.org/TR/css-cascade/#defaulting
void StyleComputer::compute_defaulted_values(ComputedProperties& style, DOM::Element const* element, Optional<CSS::PseudoElement> pseudo_element) const
{
    // Walk the list of all known CSS properties and:
    // - Add them to `style` if they are missing.
    // - Resolve `inherit` and `initial` as needed.
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = (CSS::PropertyID)i;
        compute_defaulted_property_value(style, element, property_id, pseudo_element);
    }

    // https://www.w3.org/TR/css-color-4/#resolving-other-colors
    // In the color property, the used value of currentcolor is the inherited value.
    auto const& color = style.property(CSS::PropertyID::Color);
    if (color.to_keyword() == Keyword::Currentcolor) {
        auto const& inherited_value = get_inherit_value(CSS::PropertyID::Color, element, pseudo_element);
        style.set_property(CSS::PropertyID::Color, inherited_value);
    }

    // AD-HOC: The -libweb-inherit-or-center style defaults to centering, unless a style value usually would have been
    //         inherited. This is used to support the ad-hoc default <th> text-align behavior.
    if (element && element->local_name() == HTML::TagNames::th
        && style.property(PropertyID::TextAlign).to_keyword() == Keyword::LibwebInheritOrCenter) {
        auto const* parent_element = element;
        while ((parent_element = element_to_inherit_style_from(parent_element, {}))) {
            auto parent_computed = parent_element->computed_properties();
            auto parent_cascaded = parent_element->cascaded_properties({});
            if (!parent_computed || !parent_cascaded)
                break;
            if (parent_cascaded->property(PropertyID::TextAlign)) {
                auto const& style_value = parent_computed->property(PropertyID::TextAlign);
                style.set_property(PropertyID::TextAlign, style_value, ComputedProperties::Inherited::Yes);
                break;
            }
        }
    }
}

Length::FontMetrics StyleComputer::calculate_root_element_font_metrics(ComputedProperties const& style) const
{
    auto const& root_value = style.property(CSS::PropertyID::FontSize);

    auto font_pixel_metrics = style.first_available_computed_font().pixel_metrics();
    Length::FontMetrics font_metrics { m_default_font_metrics.font_size, font_pixel_metrics };
    font_metrics.font_size = root_value.as_length().length().to_px(viewport_rect(), font_metrics, font_metrics);
    font_metrics.line_height = style.compute_line_height(viewport_rect(), font_metrics, font_metrics);

    return font_metrics;
}

RefPtr<Gfx::FontCascadeList const> StyleComputer::find_matching_font_weight_ascending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive)
{
    using Fn = AK::Function<bool(MatchingFontCandidate const&)>;
    auto pred = inclusive ? Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight >= target_weight; })
                          : Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight > target_weight; });
    auto it = find_if(candidates.begin(), candidates.end(), pred);
    for (; it != candidates.end(); ++it) {
        if (auto found_font = it->font_with_point_size(font_size_in_pt))
            return found_font;
    }
    return {};
}

RefPtr<Gfx::FontCascadeList const> StyleComputer::find_matching_font_weight_descending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive)
{
    using Fn = AK::Function<bool(MatchingFontCandidate const&)>;
    auto pred = inclusive ? Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight <= target_weight; })
                          : Fn([&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight < target_weight; });
    auto it = find_if(candidates.rbegin(), candidates.rend(), pred);
    for (; it != candidates.rend(); ++it) {
        if (auto found_font = it->font_with_point_size(font_size_in_pt))
            return found_font;
    }
    return {};
}

// Partial implementation of the font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm
// FIXME: This should be replaced by the full CSS font selection algorithm.
RefPtr<Gfx::FontCascadeList const> StyleComputer::font_matching_algorithm(FlyString const& family_name, int weight, int slope, float font_size_in_pt) const
{
    // If a font family match occurs, the user agent assembles the set of font faces in that family and then
    // narrows the set to a single face using other font properties in the order given below.
    Vector<MatchingFontCandidate> matching_family_fonts;
    for (auto const& font_key_and_loader : m_loaded_fonts) {
        if (font_key_and_loader.key.family_name.equals_ignoring_ascii_case(family_name))
            matching_family_fonts.empend(font_key_and_loader.key, const_cast<FontLoaderList*>(&font_key_and_loader.value));
    }
    Gfx::FontDatabase::the().for_each_typeface_with_family_name(family_name, [&](Gfx::Typeface const& typeface) {
        matching_family_fonts.empend(
            FontFaceKey {
                .family_name = typeface.family(),
                .weight = static_cast<int>(typeface.weight()),
                .slope = typeface.slope(),
            },
            &typeface);
    });
    quick_sort(matching_family_fonts, [](auto const& a, auto const& b) {
        return a.key.weight < b.key.weight;
    });
    // FIXME: 1. font-stretch is tried first.
    // FIXME: 2. font-style is tried next.
    // We don't have complete support of italic and oblique fonts, so matching on font-style can be simplified to:
    // If a matching slope is found, all faces which don't have that matching slope are excluded from the matching set.
    auto style_it = find_if(matching_family_fonts.begin(), matching_family_fonts.end(),
        [&](auto const& matching_font_candidate) { return matching_font_candidate.key.slope == slope; });
    if (style_it != matching_family_fonts.end()) {
        matching_family_fonts.remove_all_matching([&](auto const& matching_font_candidate) {
            return matching_font_candidate.key.slope != slope;
        });
    }
    // 3. font-weight is matched next.
    // If the desired weight is inclusively between 400 and 500, weights greater than or equal to the target weight
    // are checked in ascending order until 500 is hit and checked, followed by weights less than the target weight
    // in descending order, followed by weights greater than 500, until a match is found.
    if (weight >= 400 && weight <= 500) {
        auto it = find_if(matching_family_fonts.begin(), matching_family_fonts.end(),
            [&](auto const& matching_font_candidate) { return matching_font_candidate.key.weight >= weight; });
        for (; it != matching_family_fonts.end() && it->key.weight <= 500; ++it) {
            if (auto found_font = it->font_with_point_size(font_size_in_pt))
                return found_font;
        }
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, weight, font_size_in_pt, false))
            return found_font;
        for (; it != matching_family_fonts.end(); ++it) {
            if (auto found_font = it->font_with_point_size(font_size_in_pt))
                return found_font;
        }
    }
    // If the desired weight is less than 400, weights less than or equal to the desired weight are checked in descending order
    // followed by weights above the desired weight in ascending order until a match is found.
    if (weight < 400) {
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, weight, font_size_in_pt, true))
            return found_font;
        if (auto found_font = find_matching_font_weight_ascending(matching_family_fonts, weight, font_size_in_pt, false))
            return found_font;
    }
    // If the desired weight is greater than 500, weights greater than or equal to the desired weight are checked in ascending order
    // followed by weights below the desired weight in descending order until a match is found.
    if (weight > 500) {
        if (auto found_font = find_matching_font_weight_ascending(matching_family_fonts, weight, font_size_in_pt, true))
            return found_font;
        if (auto found_font = find_matching_font_weight_descending(matching_family_fonts, weight, font_size_in_pt, false))
            return found_font;
    }
    return {};
}

CSSPixels StyleComputer::default_user_font_size()
{
    // FIXME: This value should be configurable by the user.
    return 16;
}

// https://w3c.github.io/csswg-drafts/css-fonts/#absolute-size-mapping
CSSPixelFraction StyleComputer::absolute_size_mapping(Keyword keyword)
{
    switch (keyword) {
    case Keyword::XxSmall:
        return CSSPixels(3) / 5;
    case Keyword::XSmall:
        return CSSPixels(3) / 4;
    case Keyword::Small:
        return CSSPixels(8) / 9;
    case Keyword::Medium:
        return 1;
    case Keyword::Large:
        return CSSPixels(6) / 5;
    case Keyword::XLarge:
        return CSSPixels(3) / 2;
    case Keyword::XxLarge:
        return 2;
    case Keyword::XxxLarge:
        return 3;
    case Keyword::Smaller:
        return CSSPixels(4) / 5;
    case Keyword::Larger:
        return CSSPixels(5) / 4;
    default:
        return 1;
    }
}

RefPtr<Gfx::FontCascadeList const> StyleComputer::compute_font_for_style_values(DOM::Element const* element, Optional<CSS::PseudoElement> pseudo_element, CSSStyleValue const& font_family, CSSStyleValue const& font_size, CSSStyleValue const& font_style, CSSStyleValue const& font_weight, CSSStyleValue const& font_stretch, int math_depth) const
{
    auto* parent_element = element_to_inherit_style_from(element, pseudo_element);

    auto width = font_stretch.to_font_width();
    auto weight = font_weight.to_font_weight();

    auto font_size_in_px = default_user_font_size();

    Gfx::FontPixelMetrics font_pixel_metrics;
    if (parent_element && parent_element->computed_properties())
        font_pixel_metrics = parent_element->computed_properties()->first_available_computed_font().pixel_metrics();
    else
        font_pixel_metrics = Platform::FontPlugin::the().default_font(font_size_in_px.to_float())->pixel_metrics();
    auto parent_font_size = [&]() -> CSSPixels {
        if (!parent_element || !parent_element->computed_properties())
            return font_size_in_px;
        auto const& value = parent_element->computed_properties()->property(CSS::PropertyID::FontSize);
        if (value.is_length()) {
            auto length = value.as_length().length();
            if (length.is_absolute() || length.is_relative()) {
                Length::FontMetrics font_metrics { font_size_in_px, font_pixel_metrics };
                return length.to_px(viewport_rect(), font_metrics, root_element_font_metrics_for_element(element));
            }
        }
        return font_size_in_px;
    }();

    if (font_size.is_keyword()) {
        auto const keyword = font_size.to_keyword();

        if (keyword == Keyword::Math) {
            auto math_scaling_factor = [&]() {
                // https://w3c.github.io/mathml-core/#the-math-script-level-property
                // If the specified value font-size is math then the computed value of font-size is obtained by multiplying
                // the inherited value of font-size by a nonzero scale factor calculated by the following procedure:
                // 1. Let A be the inherited math-depth value, B the computed math-depth value, C be 0.71 and S be 1.0
                int inherited_math_depth = parent_element && parent_element->computed_properties()
                    ? parent_element->computed_properties()->math_depth()
                    : InitialValues::math_depth();
                int computed_math_depth = math_depth;
                auto size_ratio = 0.71;
                auto scale = 1.0;
                // 2. If A = B then return S.
                bool invert_scale_factor = false;
                if (inherited_math_depth == computed_math_depth) {
                    return scale;
                }
                //    If B < A, swap A and B and set InvertScaleFactor to true.
                else if (computed_math_depth < inherited_math_depth) {
                    AK::swap(inherited_math_depth, computed_math_depth);
                    invert_scale_factor = true;
                }
                //    Otherwise B > A and set InvertScaleFactor to false.
                else {
                    invert_scale_factor = false;
                }
                // 3. Let E be B - A > 0.
                double e = (computed_math_depth - inherited_math_depth) > 0;
                // FIXME: 4. If the inherited first available font has an OpenType MATH table:
                //    - If A ≤ 0 and B ≥ 2 then multiply S by scriptScriptPercentScaleDown and decrement E by 2.
                //    - Otherwise if A = 1 then multiply S by scriptScriptPercentScaleDown / scriptPercentScaleDown and decrement E by 1.
                //    - Otherwise if B = 1 then multiply S by scriptPercentScaleDown and decrement E by 1.
                // 5. Multiply S by C^E.
                scale *= AK::pow(size_ratio, e);
                // 6. Return S if InvertScaleFactor is false and 1/S otherwise.
                if (!invert_scale_factor)
                    return scale;
                return 1.0 / scale;
            };
            font_size_in_px = parent_font_size.scale_by(math_scaling_factor());
        } else {
            // https://w3c.github.io/csswg-drafts/css-fonts/#valdef-font-size-relative-size
            // TODO: If the parent element has a keyword font size in the absolute size keyword mapping table,
            //       larger may compute the font size to the next entry in the table,
            //       and smaller may compute the font size to the previous entry in the table.
            if (keyword == Keyword::Smaller || keyword == Keyword::Larger) {
                if (parent_element && parent_element->computed_properties()) {
                    font_size_in_px = CSSPixels::nearest_value_for(parent_element->computed_properties()->first_available_computed_font().pixel_metrics().size);
                }
            }
            font_size_in_px *= absolute_size_mapping(keyword);
        }
    } else {
        Length::ResolutionContext const length_resolution_context {
            .viewport_rect = viewport_rect(),
            .font_metrics = Length::FontMetrics { parent_font_size, font_pixel_metrics },
            .root_font_metrics = root_element_font_metrics_for_element(element),
        };

        Optional<Length> maybe_length;
        if (font_size.is_percentage()) {
            // Percentages refer to parent element's font size
            maybe_length = Length::make_px(CSSPixels::nearest_value_for(font_size.as_percentage().percentage().as_fraction() * parent_font_size.to_double()));

        } else if (font_size.is_length()) {
            maybe_length = font_size.as_length().length();
        } else if (font_size.is_calculated()) {
            maybe_length = font_size.as_calculated().resolve_length_deprecated({
                .percentage_basis = Length::make_px(parent_font_size),
                .length_resolution_context = length_resolution_context,
            });
        }
        if (maybe_length.has_value()) {
            font_size_in_px = maybe_length.value().to_px(length_resolution_context);
        }
    }

    auto slope = font_style.to_font_slope();

    // FIXME: Implement the full font-matching algorithm: https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm

    float const font_size_in_pt = font_size_in_px * 0.75f;

    auto find_font = [&](FlyString const& family) -> RefPtr<Gfx::FontCascadeList const> {
        FontFaceKey key {
            .family_name = family,
            .weight = weight,
            .slope = slope,
        };
        auto result = Gfx::FontCascadeList::create();
        if (auto it = m_loaded_fonts.find(key); it != m_loaded_fonts.end()) {
            auto const& loaders = it->value;
            for (auto const& loader : loaders) {
                if (auto found_font = loader->font_with_point_size(font_size_in_pt))
                    result->add(*found_font, loader->unicode_ranges());
            }
            return result;
        }

        if (auto found_font = font_matching_algorithm(family, weight, slope, font_size_in_pt); found_font && !found_font->is_empty()) {
            return found_font;
        }

        if (auto found_font = Gfx::FontDatabase::the().get(family, font_size_in_pt, weight, width, slope)) {
            result->add(*found_font);
            return result;
        }

        return {};
    };

    auto find_generic_font = [&](Keyword font_id) -> RefPtr<Gfx::FontCascadeList const> {
        Platform::GenericFont generic_font {};
        switch (font_id) {
        case Keyword::Monospace:
        case Keyword::UiMonospace:
            generic_font = Platform::GenericFont::Monospace;
            break;
        case Keyword::Serif:
            generic_font = Platform::GenericFont::Serif;
            break;
        case Keyword::Fantasy:
            generic_font = Platform::GenericFont::Fantasy;
            break;
        case Keyword::SansSerif:
            generic_font = Platform::GenericFont::SansSerif;
            break;
        case Keyword::Cursive:
            generic_font = Platform::GenericFont::Cursive;
            break;
        case Keyword::UiSerif:
            generic_font = Platform::GenericFont::UiSerif;
            break;
        case Keyword::UiSansSerif:
            generic_font = Platform::GenericFont::UiSansSerif;
            break;
        case Keyword::UiRounded:
            generic_font = Platform::GenericFont::UiRounded;
            break;
        default:
            return {};
        }
        return find_font(Platform::FontPlugin::the().generic_font_name(generic_font));
    };

    auto font_list = Gfx::FontCascadeList::create();
    if (font_family.is_value_list()) {
        auto const& family_list = static_cast<StyleValueList const&>(font_family).values();
        for (auto const& family : family_list) {
            RefPtr<Gfx::FontCascadeList const> other_font_list;
            if (family->is_keyword()) {
                other_font_list = find_generic_font(family->to_keyword());
            } else if (family->is_string()) {
                other_font_list = find_font(family->as_string().string_value());
            } else if (family->is_custom_ident()) {
                other_font_list = find_font(family->as_custom_ident().custom_ident());
            }
            if (other_font_list)
                font_list->extend(*other_font_list);
        }
    } else if (font_family.is_keyword()) {
        if (auto other_font_list = find_generic_font(font_family.to_keyword()))
            font_list->extend(*other_font_list);
    } else if (font_family.is_string()) {
        if (auto other_font_list = find_font(font_family.as_string().string_value()))
            font_list->extend(*other_font_list);
    } else if (font_family.is_custom_ident()) {
        if (auto other_font_list = find_font(font_family.as_custom_ident().custom_ident()))
            font_list->extend(*other_font_list);
    }

    auto default_font = Platform::FontPlugin::the().default_font(font_size_in_pt);
    if (font_list->is_empty()) {
        // This is needed to make sure we check default font before reaching to emojis.
        font_list->add(*default_font);
    }

    if (auto emoji_font = Platform::FontPlugin::the().default_emoji_font(font_size_in_pt); emoji_font) {
        font_list->add(*emoji_font);
    }

    // The default font is already included in the font list, but we explicitly set it
    // as the last-resort font. This ensures that if none of the specified fonts contain
    // the requested code point, there is still a font available to provide a fallback glyph.
    font_list->set_last_resort_font(*default_font);

    return font_list;
}

void StyleComputer::compute_font(ComputedProperties& style, DOM::Element const* element, Optional<CSS::PseudoElement> pseudo_element) const
{
    // To compute the font, first ensure that we've defaulted the relevant CSS font properties.
    // FIXME: This should be more sophisticated.
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontFamily, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontSize, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontWidth, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontStyle, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontWeight, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::LineHeight, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariant, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantAlternates, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantCaps, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantEmoji, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantEastAsian, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantLigatures, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantNumeric, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::FontVariantPosition, pseudo_element);

    auto const& font_family = style.property(CSS::PropertyID::FontFamily);
    auto const& font_size = style.property(CSS::PropertyID::FontSize);
    auto const& font_style = style.property(CSS::PropertyID::FontStyle);
    auto const& font_weight = style.property(CSS::PropertyID::FontWeight);
    auto const& font_width = style.property(CSS::PropertyID::FontWidth);

    auto font_list = compute_font_for_style_values(element, pseudo_element, font_family, font_size, font_style, font_weight, font_width, style.math_depth());
    VERIFY(font_list);
    VERIFY(!font_list->is_empty());

    RefPtr<Gfx::Font const> const found_font = font_list->first();

    style.set_property(
        CSS::PropertyID::FontSize,
        LengthStyleValue::create(CSS::Length::make_px(CSSPixels::nearest_value_for(found_font->pixel_size()))),
        style.is_property_inherited(CSS::PropertyID::FontSize) ? ComputedProperties::Inherited::Yes : ComputedProperties::Inherited::No);
    style.set_property(
        CSS::PropertyID::FontWeight,
        NumberStyleValue::create(font_weight.to_font_weight()),
        style.is_property_inherited(CSS::PropertyID::FontWeight) ? ComputedProperties::Inherited::Yes : ComputedProperties::Inherited::No);

    style.set_computed_font_list(*font_list);

    if (element && is<HTML::HTMLHtmlElement>(*element)) {
        const_cast<StyleComputer&>(*this).m_root_element_font_metrics = calculate_root_element_font_metrics(style);
    }
}

LogicalAliasMappingContext StyleComputer::compute_logical_alias_mapping_context(DOM::Element& element, Optional<PseudoElement> pseudo_element, ComputeStyleMode mode, MatchingRuleSet const& matching_rule_set) const
{
    auto normalize_value = [&](auto property_id, auto value) {
        if (!value || value->is_inherit() || value->is_unset()) {
            if (auto const* inheritance_parent = element_to_inherit_style_from(&element, pseudo_element)) {
                value = inheritance_parent->computed_properties()->property(property_id);
            } else {
                value = property_initial_value(property_id);
            }
        }

        if (value->is_initial())
            value = property_initial_value(property_id);

        return value;
    };

    bool did_match_any_pseudo_element_rules = false;

    static Array<PropertyID, 2> properties_to_cascade {
        PropertyID::WritingMode,
        PropertyID::Direction,
    };
    auto cascaded_properties = compute_cascaded_values(
        element,
        pseudo_element,
        did_match_any_pseudo_element_rules,
        mode, matching_rule_set,
        {},
        properties_to_cascade);

    auto writing_mode = normalize_value(PropertyID::WritingMode, cascaded_properties->property(PropertyID::WritingMode));
    auto direction = normalize_value(PropertyID::Direction, cascaded_properties->property(PropertyID::Direction));

    return LogicalAliasMappingContext {
        .writing_mode = keyword_to_writing_mode(writing_mode->to_keyword()).release_value(),
        .direction = keyword_to_direction(direction->to_keyword()).release_value()
    };
}

Gfx::Font const& StyleComputer::initial_font() const
{
    // FIXME: This is not correct.
    static auto font = ComputedProperties::font_fallback(false, false, 12);
    return font;
}

void StyleComputer::absolutize_values(ComputedProperties& style, GC::Ptr<DOM::Element const> element) const
{
    Length::FontMetrics font_metrics {
        root_element_font_metrics_for_element(element).font_size,
        style.first_available_computed_font().pixel_metrics()
    };

    // "A percentage value specifies an absolute font size relative to the parent element’s computed font-size. Negative percentages are invalid."
    auto& font_size_value_slot = style.m_property_values[to_underlying(CSS::PropertyID::FontSize)];
    if (font_size_value_slot && font_size_value_slot->is_percentage()) {
        auto parent_font_size = get_inherit_value(CSS::PropertyID::FontSize, element)->as_length().length().to_px(viewport_rect(), font_metrics, m_root_element_font_metrics);
        font_size_value_slot = LengthStyleValue::create(
            Length::make_px(CSSPixels::nearest_value_for(parent_font_size * font_size_value_slot->as_percentage().percentage().as_fraction())));
    }

    auto font_size = font_size_value_slot->as_length().length().to_px(viewport_rect(), font_metrics, m_root_element_font_metrics);
    font_metrics.font_size = font_size;
    style.set_font_size({}, font_size);

    // NOTE: Percentage line-height values are relative to the font-size of the element.
    //       We have to resolve them right away, so that the *computed* line-height is ready for inheritance.
    //       We can't simply absolutize *all* percentage values against the font size,
    //       because most percentages are relative to containing block metrics.
    auto& line_height_value_slot = style.m_property_values[to_underlying(CSS::PropertyID::LineHeight)];
    if (line_height_value_slot && line_height_value_slot->is_percentage()) {
        line_height_value_slot = LengthStyleValue::create(
            Length::make_px(CSSPixels::nearest_value_for(font_size * static_cast<double>(line_height_value_slot->as_percentage().percentage().as_fraction()))));
    }

    auto line_height = style.compute_line_height(viewport_rect(), font_metrics, m_root_element_font_metrics);
    font_metrics.line_height = line_height;

    // NOTE: line-height might be using lh which should be resolved against the parent line height (like we did here already)
    if (line_height_value_slot && line_height_value_slot->is_length())
        line_height_value_slot = LengthStyleValue::create(Length::make_px(line_height));

    for (size_t i = 0; i < style.m_property_values.size(); ++i) {
        auto& value_slot = style.m_property_values[i];
        if (!value_slot)
            continue;
        value_slot = value_slot->absolutized(viewport_rect(), font_metrics, m_root_element_font_metrics);
    }

    style.set_line_height({}, line_height);
}

void StyleComputer::resolve_effective_overflow_values(ComputedProperties& style) const
{
    // https://www.w3.org/TR/css-overflow-3/#overflow-control
    // The visible/clip values of overflow compute to auto/hidden (respectively) if one of overflow-x or
    // overflow-y is neither visible nor clip.
    auto overflow_x = keyword_to_overflow(style.property(PropertyID::OverflowX).to_keyword());
    auto overflow_y = keyword_to_overflow(style.property(PropertyID::OverflowY).to_keyword());
    auto overflow_x_is_visible_or_clip = overflow_x == Overflow::Visible || overflow_x == Overflow::Clip;
    auto overflow_y_is_visible_or_clip = overflow_y == Overflow::Visible || overflow_y == Overflow::Clip;
    if (!overflow_x_is_visible_or_clip || !overflow_y_is_visible_or_clip) {
        if (overflow_x == CSS::Overflow::Visible)
            style.set_property(CSS::PropertyID::OverflowX, CSSKeywordValue::create(Keyword::Auto));
        if (overflow_x == CSS::Overflow::Clip)
            style.set_property(CSS::PropertyID::OverflowX, CSSKeywordValue::create(Keyword::Hidden));
        if (overflow_y == CSS::Overflow::Visible)
            style.set_property(CSS::PropertyID::OverflowY, CSSKeywordValue::create(Keyword::Auto));
        if (overflow_y == CSS::Overflow::Clip)
            style.set_property(CSS::PropertyID::OverflowY, CSSKeywordValue::create(Keyword::Hidden));
    }
}

static void compute_text_align(ComputedProperties& style, DOM::Element const& element, Optional<PseudoElement> pseudo_element)
{
    // https://drafts.csswg.org/css-text-4/#valdef-text-align-match-parent
    // This value behaves the same as inherit (computes to its parent’s computed value) except that an inherited
    // value of start or end is interpreted against the parent’s direction value and results in a computed value of
    // either left or right. Computes to start when specified on the root element.
    if (style.property(PropertyID::TextAlign).to_keyword() == Keyword::MatchParent) {

        // If it's a pseudo-element, then the "parent" is the originating element instead.
        auto const* parent = [&]() -> DOM::Element const* {
            if (pseudo_element.has_value())
                return &element;
            return element.parent_element();
        }();

        if (parent) {
            auto const& parent_text_align = parent->computed_properties()->property(PropertyID::TextAlign);
            auto const& parent_direction = parent->computed_properties()->direction();
            switch (parent_text_align.to_keyword()) {
            case Keyword::Start:
                if (parent_direction == Direction::Ltr) {
                    style.set_property(PropertyID::TextAlign, CSSKeywordValue::create(Keyword::Left));
                } else {
                    style.set_property(PropertyID::TextAlign, CSSKeywordValue::create(Keyword::Right));
                }
                break;

            case Keyword::End:
                if (parent_direction == Direction::Ltr) {
                    style.set_property(PropertyID::TextAlign, CSSKeywordValue::create(Keyword::Right));
                } else {
                    style.set_property(PropertyID::TextAlign, CSSKeywordValue::create(Keyword::Left));
                }
                break;

            default:
                style.set_property(PropertyID::TextAlign, parent_text_align);
            }
        } else {
            style.set_property(PropertyID::TextAlign, CSSKeywordValue::create(Keyword::Start));
        }
    }
}

enum class BoxTypeTransformation {
    None,
    Blockify,
    Inlinify,
};

static BoxTypeTransformation required_box_type_transformation(ComputedProperties const& style, DOM::Element const& element, Optional<CSS::PseudoElement> const& pseudo_element)
{
    // NOTE: We never blockify <br> elements. They are always inline.
    //       There is currently no way to express in CSS how a <br> element really behaves.
    //       Spec issue: https://github.com/whatwg/html/issues/2291
    if (is<HTML::HTMLBRElement>(element))
        return BoxTypeTransformation::None;

    // Absolute positioning or floating an element blockifies the box’s display type. [CSS2]
    if (style.position() == CSS::Positioning::Absolute || style.position() == CSS::Positioning::Fixed || style.float_() != CSS::Float::None)
        return BoxTypeTransformation::Blockify;

    // FIXME: Containment in a ruby container inlinifies the box’s display type, as described in [CSS-RUBY-1].

    // NOTE: If we're computing style for a pseudo-element, the effective parent will be the originating element itself, not its parent.
    auto parent = pseudo_element.has_value() ? GC::Ptr<DOM::Element const> { &element } : element.parent_element();

    // A parent with a grid or flex display value blockifies the box’s display type. [CSS-GRID-1] [CSS-FLEXBOX-1]
    if (parent && parent->computed_properties()) {
        auto const& parent_display = parent->computed_properties()->display();
        if (parent_display.is_grid_inside() || parent_display.is_flex_inside())
            return BoxTypeTransformation::Blockify;
    }

    return BoxTypeTransformation::None;
}

// https://drafts.csswg.org/css-display/#transformations
void StyleComputer::transform_box_type_if_needed(ComputedProperties& style, DOM::Element const& element, Optional<CSS::PseudoElement> pseudo_element) const
{
    // 2.7. Automatic Box Type Transformations

    // Some layout effects require blockification or inlinification of the box type,
    // which sets the box’s computed outer display type to block or inline (respectively).
    // (This has no effect on display types that generate no box at all, such as none or contents.)

    auto display = style.display();

    if (display.is_none() || (display.is_contents() && !element.is_document_element()))
        return;

    // https://drafts.csswg.org/css-display/#root
    // The root element’s display type is always blockified, and its principal box always establishes an independent formatting context.
    if (element.is_document_element() && !display.is_block_outside()) {
        style.set_property(CSS::PropertyID::Display, DisplayStyleValue::create(Display::from_short(CSS::Display::Short::Block)));
        return;
    }

    auto new_display = display;

    if (display.is_math_inside()) {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // For elements that are not MathML elements, if the specified value of display is inline math or block math
        // then the computed value is block flow and inline flow respectively.
        if (element.namespace_uri() != Namespace::MathML)
            new_display = CSS::Display { display.outside(), CSS::DisplayInside::Flow };
        // For the mtable element the computed value is block table and inline table respectively.
        else if (element.tag_name().equals_ignoring_ascii_case("mtable"sv))
            new_display = CSS::Display { display.outside(), CSS::DisplayInside::Table };
        // For the mtr element, the computed value is table-row.
        else if (element.tag_name().equals_ignoring_ascii_case("mtr"sv))
            new_display = CSS::Display { CSS::DisplayInternal::TableRow };
        // For the mtd element, the computed value is table-cell.
        else if (element.tag_name().equals_ignoring_ascii_case("mtd"sv))
            new_display = CSS::Display { CSS::DisplayInternal::TableCell };
    }

    switch (required_box_type_transformation(style, element, pseudo_element)) {
    case BoxTypeTransformation::None:
        break;
    case BoxTypeTransformation::Blockify:
        if (display.is_block_outside())
            return;
        // If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container.
        if (display.is_internal()) {
            new_display = CSS::Display::from_short(CSS::Display::Short::Block);
        } else {
            VERIFY(display.is_outside_and_inside());

            // For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root nature).
            // For consistency, a run-in flow-root box also blockifies to a block box.
            if (display.is_inline_block()) {
                new_display = CSS::Display { CSS::DisplayOutside::Block, CSS::DisplayInside::Flow, display.list_item() };
            } else {
                new_display = CSS::Display { CSS::DisplayOutside::Block, display.inside(), display.list_item() };
            }
        }
        break;
    case BoxTypeTransformation::Inlinify:
        if (display.is_inline_outside()) {
            // FIXME: If an inline box (inline flow) is inlinified, it recursively inlinifies all of its in-flow children,
            //        so that no block-level descendants break up the inline formatting context in which it participates.
            if (display.is_flow_inside()) {
                dbgln("FIXME: Inlinify inline box children recursively");
            }
            break;
        }
        if (display.is_internal()) {
            // Inlinification has no effect on layout-internal boxes. (However, placement in such an inline context will typically cause them
            // to be wrapped in an appropriately-typed anonymous inline-level box.)
        } else {
            VERIFY(display.is_outside_and_inside());

            // If a block box (block flow) is inlinified, its inner display type is set to flow-root so that it remains a block container.
            if (display.is_block_outside() && display.is_flow_inside()) {
                new_display = CSS::Display { CSS::DisplayOutside::Inline, CSS::DisplayInside::FlowRoot, display.list_item() };
            }

            new_display = CSS::Display { CSS::DisplayOutside::Inline, display.inside(), display.list_item() };
        }
        break;
    }

    if (new_display != display)
        style.set_property(CSS::PropertyID::Display, DisplayStyleValue::create(new_display));
}

GC::Ref<ComputedProperties> StyleComputer::create_document_style() const
{
    auto style = document().heap().allocate<CSS::ComputedProperties>();
    compute_math_depth(style, nullptr, {});
    compute_font(style, nullptr, {});
    compute_defaulted_values(style, nullptr, {});
    absolutize_values(style, nullptr);
    style->set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().width())));
    style->set_property(CSS::PropertyID::Height, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().height())));
    style->set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Block)));
    return style;
}

GC::Ref<ComputedProperties> StyleComputer::compute_style(DOM::Element& element, Optional<CSS::PseudoElement> pseudo_element, Optional<bool&> did_change_custom_properties) const
{
    return *compute_style_impl(element, move(pseudo_element), ComputeStyleMode::Normal, did_change_custom_properties);
}

GC::Ptr<ComputedProperties> StyleComputer::compute_pseudo_element_style_if_needed(DOM::Element& element, Optional<CSS::PseudoElement> pseudo_element, Optional<bool&> did_change_custom_properties) const
{
    return compute_style_impl(element, move(pseudo_element), ComputeStyleMode::CreatePseudoElementStyleIfNeeded, did_change_custom_properties);
}

GC::Ptr<ComputedProperties> StyleComputer::compute_style_impl(DOM::Element& element, Optional<CSS::PseudoElement> pseudo_element, ComputeStyleMode mode, Optional<bool&> did_change_custom_properties) const
{
    build_rule_cache_if_needed();

    // Special path for elements that use pseudo element as style selector
    if (element.use_pseudo_element().has_value()) {
        auto& parent_element = as<HTML::HTMLElement>(*element.root().parent_or_shadow_host());
        auto style = compute_style(parent_element, *element.use_pseudo_element());

        // Merge back inline styles
        if (auto inline_style = element.inline_style()) {
            for (auto const& property : inline_style->properties())
                style->set_property(property.property_id, property.value);
        }
        return style;
    }

    ScopeGuard guard { [&element]() { element.set_needs_style_update(false); } };

    // 1. Perform the cascade. This produces the "specified style"
    bool did_match_any_pseudo_element_rules = false;
    PseudoClassBitmap attempted_pseudo_class_matches;
    auto matching_rule_set = build_matching_rule_set(element, pseudo_element, attempted_pseudo_class_matches, did_match_any_pseudo_element_rules, mode);

    DOM::AbstractElement abstract_element { element, pseudo_element };
    auto old_custom_properties = abstract_element.custom_properties();

    // Resolve all the CSS custom properties ("variables") for this element:
    // FIXME: Also resolve !important custom properties, in a second cascade.
    if (!pseudo_element.has_value() || pseudo_element_supports_property(*pseudo_element, PropertyID::Custom)) {
        HashMap<FlyString, CSS::StyleProperty> custom_properties;
        for (auto& layer : matching_rule_set.author_rules) {
            cascade_custom_properties(element, pseudo_element, layer.rules, custom_properties);
        }
        element.set_custom_properties(pseudo_element, move(custom_properties));
    }

    auto logical_alias_mapping_context = compute_logical_alias_mapping_context(element, pseudo_element, mode, matching_rule_set);
    auto cascaded_properties = compute_cascaded_values(element, pseudo_element, did_match_any_pseudo_element_rules, mode, matching_rule_set, logical_alias_mapping_context, {});
    element.set_cascaded_properties(pseudo_element, cascaded_properties);

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        // NOTE: If we're computing style for a pseudo-element, we look for a number of reasons to bail early.

        // Bail if no pseudo-element rules matched.
        if (!did_match_any_pseudo_element_rules)
            return {};

        // Bail if no pseudo-element would be generated due to...
        // - content: none
        // - content: normal (for ::before and ::after)
        bool content_is_normal = false;
        if (auto content_value = cascaded_properties->property(CSS::PropertyID::Content)) {
            if (content_value->is_keyword()) {
                auto content = content_value->as_keyword().keyword();
                if (content == CSS::Keyword::None)
                    return {};
                content_is_normal = content == CSS::Keyword::Normal;
            } else {
                content_is_normal = false;
            }
        } else {
            // NOTE: `normal` is the initial value, so the absence of a value is treated as `normal`.
            content_is_normal = true;
        }
        if (content_is_normal && first_is_one_of(*pseudo_element, CSS::PseudoElement::Before, CSS::PseudoElement::After)) {
            return {};
        }
    }

    auto computed_properties = compute_properties(element, pseudo_element, cascaded_properties);
    computed_properties->set_attempted_pseudo_class_matches(attempted_pseudo_class_matches);

    if (did_change_custom_properties.has_value() && abstract_element.custom_properties() != old_custom_properties) {
        *did_change_custom_properties = true;
    }

    return computed_properties;
}

static bool is_monospace(CSSStyleValue const& value)
{
    if (value.to_keyword() == Keyword::Monospace)
        return true;
    if (value.is_value_list()) {
        auto const& values = value.as_value_list().values();
        if (values.size() == 1 && values[0]->to_keyword() == Keyword::Monospace)
            return true;
    }
    return false;
}

// HACK: This function implements time-travelling inheritance for the font-size property
//       in situations where the cascade ended up with `font-family: monospace`.
//       In such cases, other browsers will magically change the meaning of keyword font sizes
//       *even in earlier stages of the cascade!!* to be relative to the default monospace font size (13px)
//       instead of the default font size (16px).
//       See this blog post for a lot more details about this weirdness:
//       https://manishearth.github.io/blog/2017/08/10/font-size-an-unexpectedly-complex-css-property/
RefPtr<CSSStyleValue const> StyleComputer::recascade_font_size_if_needed(
    DOM::Element& element,
    Optional<CSS::PseudoElement> pseudo_element,
    CascadedProperties& cascaded_properties) const
{
    // Check for `font-family: monospace`. Note that `font-family: monospace, AnythingElse` does not trigger this path.
    // Some CSS frameworks use `font-family: monospace, monospace` to work around this behavior.
    auto font_family_value = cascaded_properties.property(CSS::PropertyID::FontFamily);
    if (!font_family_value || !is_monospace(*font_family_value))
        return nullptr;

    // FIXME: This should be configurable.
    constexpr CSSPixels default_monospace_font_size_in_px = 13;
    static auto monospace_font_family_name = Platform::FontPlugin::the().generic_font_name(Platform::GenericFont::Monospace);
    static auto monospace_font = Gfx::FontDatabase::the().get(monospace_font_family_name, default_monospace_font_size_in_px * 0.75f, 400, Gfx::FontWidth::Normal, 0);

    // Reconstruct the line of ancestor elements we need to inherit style from, and then do the cascade again
    // but only for the font-size property.
    Vector<DOM::Element&> ancestors;
    if (pseudo_element.has_value())
        ancestors.append(element);
    for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element())
        ancestors.append(*ancestor);

    NonnullRefPtr<CSSStyleValue const> new_font_size = CSS::LengthStyleValue::create(CSS::Length::make_px(default_monospace_font_size_in_px));
    CSSPixels current_size_in_px = default_monospace_font_size_in_px;

    for (auto& ancestor : ancestors.in_reverse()) {
        auto& ancestor_cascaded_properties = *ancestor.cascaded_properties({});
        auto font_size_value = ancestor_cascaded_properties.property(CSS::PropertyID::FontSize);

        if (!font_size_value)
            continue;
        if (font_size_value->is_initial() || font_size_value->is_unset()) {
            current_size_in_px = default_monospace_font_size_in_px;
            continue;
        }
        if (font_size_value->is_inherit()) {
            // Do nothing.
            continue;
        }

        if (font_size_value->is_keyword()) {
            current_size_in_px = default_monospace_font_size_in_px * absolute_size_mapping(font_size_value->to_keyword());
            continue;
        }

        if (font_size_value->is_percentage()) {
            current_size_in_px = CSSPixels::nearest_value_for(font_size_value->as_percentage().percentage().as_fraction() * current_size_in_px);
            continue;
        }

        if (font_size_value->is_calculated()) {
            dbgln("FIXME: Support calc() when time-traveling for monospace font-size");
            continue;
        }

        VERIFY(font_size_value->is_length());
        current_size_in_px = font_size_value->as_length().length().to_px(viewport_rect(), Length::FontMetrics { current_size_in_px, monospace_font->with_size(current_size_in_px * 0.75f)->pixel_metrics() }, m_root_element_font_metrics);
    };

    return CSS::LengthStyleValue::create(CSS::Length::make_px(current_size_in_px));
}

GC::Ref<ComputedProperties> StyleComputer::compute_properties(DOM::Element& element, Optional<PseudoElement> pseudo_element, CascadedProperties& cascaded_properties) const
{
    DOM::AbstractElement abstract_element { element, pseudo_element };
    auto computed_style = document().heap().allocate<CSS::ComputedProperties>();

    auto new_font_size = recascade_font_size_if_needed(element, pseudo_element, cascaded_properties);
    if (new_font_size)
        computed_style->set_property(PropertyID::FontSize, *new_font_size, ComputedProperties::Inherited::No, Important::No);

    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<CSS::PropertyID>(i);
        auto value = cascaded_properties.property(property_id);
        auto inherited = ComputedProperties::Inherited::No;

        // NOTE: We've already handled font-size above.
        if (property_id == PropertyID::FontSize && !value && new_font_size)
            continue;

        // FIXME: Logical properties should inherit from their parent's equivalent unmapped logical property.
        if ((!value && is_inherited_property(property_id))
            || (value && value->is_inherit())) {
            if (auto inheritance_parent = element_to_inherit_style_from(&element, pseudo_element)) {
                value = inheritance_parent->computed_properties()->property(property_id);
                inherited = ComputedProperties::Inherited::Yes;
            } else {
                value = property_initial_value(property_id);
            }
        }

        if (!value || value->is_initial())
            value = property_initial_value(property_id);

        if (value->is_unset()) {
            if (is_inherited_property(property_id))
                value = CSSKeywordValue::create(Keyword::Inherit);
            else
                value = CSSKeywordValue::create(Keyword::Initial);
        }

        computed_style->set_property(property_id, value.release_nonnull(), inherited);

        if (property_id == PropertyID::AnimationName) {
            computed_style->set_animation_name_source(cascaded_properties.property_source(property_id));
        }
        if (property_id == PropertyID::TransitionProperty) {
            computed_style->set_transition_property_source(cascaded_properties.property_source(property_id));
        }
    }

    // Animation declarations [css-animations-2]
    auto animation_name = [&]() -> Optional<String> {
        auto const animation_name = computed_style->maybe_null_property(PropertyID::AnimationName);
        if (!animation_name)
            return OptionalNone {};
        if (animation_name->is_keyword() && animation_name->to_keyword() == Keyword::None)
            return OptionalNone {};
        if (animation_name->is_string())
            return animation_name->as_string().string_value().to_string();
        return animation_name->to_string(SerializationMode::Normal);
    }();

    if (animation_name.has_value()) {
        if (auto source_declaration = computed_style->animation_name_source()) {
            auto& realm = element.realm();

            if (source_declaration != element.cached_animation_name_source(pseudo_element)) {
                // This animation name is new, so we need to create a new animation for it.
                if (auto existing_animation = element.cached_animation_name_animation(pseudo_element))
                    existing_animation->cancel(Animations::Animation::ShouldInvalidate::No);
                element.set_cached_animation_name_source(source_declaration, pseudo_element);

                auto effect = Animations::KeyframeEffect::create(realm);
                auto animation = CSSAnimation::create(realm);
                animation->set_id(animation_name.release_value());
                animation->set_timeline(m_document->timeline());
                animation->set_owning_element(element);
                animation->set_effect(effect);
                apply_animation_properties(m_document, cascaded_properties, animation);
                if (pseudo_element.has_value())
                    effect->set_pseudo_element(Selector::PseudoElementSelector { pseudo_element.value() });

                if (auto* rule_cache = rule_cache_for_cascade_origin(CascadeOrigin::Author, {}, {})) {
                    if (auto keyframe_set = rule_cache->rules_by_animation_keyframes.get(animation->id()); keyframe_set.has_value())
                        effect->set_key_frame_set(keyframe_set.value());
                }

                effect->set_target(&element);
                element.set_cached_animation_name_animation(animation, pseudo_element);
            } else {
                // The animation hasn't changed, but some properties of the animation may have
                if (auto animation = element.cached_animation_name_animation(pseudo_element); animation)
                    apply_animation_properties(m_document, cascaded_properties, *animation);
            }
        }
    } else {
        // If the element had an existing animation, cancel it
        if (auto existing_animation = element.cached_animation_name_animation(pseudo_element)) {
            existing_animation->cancel(Animations::Animation::ShouldInvalidate::No);
            element.set_cached_animation_name_animation({}, pseudo_element);
            element.set_cached_animation_name_source({}, pseudo_element);
        }
    }

    auto animations = element.get_animations_internal(Animations::GetAnimationsOptions { .subtree = false });
    if (animations.is_exception()) {
        dbgln("Error getting animations for element {}", element.debug_description());
    } else {
        for (auto& animation : animations.value()) {
            if (auto effect = animation->effect(); effect && effect->is_keyframe_effect()) {
                auto& keyframe_effect = *static_cast<Animations::KeyframeEffect*>(effect.ptr());
                if (keyframe_effect.pseudo_element_type() == pseudo_element)
                    collect_animation_into(element, pseudo_element, keyframe_effect, computed_style);
            }
        }
    }

    // Compute the value of custom properties
    compute_custom_properties(computed_style, abstract_element);

    // 2. Compute the math-depth property, since that might affect the font-size
    compute_math_depth(computed_style, &element, pseudo_element);

    // 3. Compute the font, since that may be needed for font-relative CSS units
    compute_font(computed_style, &element, pseudo_element);

    // 4. Absolutize values, turning font/viewport relative lengths into absolute lengths
    absolutize_values(computed_style, element);

    // 5. Default the values, applying inheritance and 'initial' as needed
    compute_defaulted_values(computed_style, &element, pseudo_element);

    // 6. Run automatic box type transformations
    transform_box_type_if_needed(computed_style, element, pseudo_element);

    // 7. Apply any property-specific computed value logic
    resolve_effective_overflow_values(computed_style);
    compute_text_align(computed_style, element, pseudo_element);

    // 8. Let the element adjust computed style
    element.adjust_computed_style(computed_style);

    // 9. Transition declarations [css-transitions-1]
    // Theoretically this should be part of the cascade, but it works with computed values, which we don't have until now.
    compute_transitioned_properties(computed_style, element, pseudo_element);
    if (auto previous_style = element.computed_properties(pseudo_element)) {
        start_needed_transitions(*previous_style, computed_style, element, pseudo_element);
    }

    return computed_style;
}

void StyleComputer::build_rule_cache_if_needed() const
{
    if (has_valid_rule_cache())
        return;
    const_cast<StyleComputer&>(*this).build_rule_cache();
}

struct SimplifiedSelectorForBucketing {
    CSS::Selector::SimpleSelector::Type type;
    FlyString name;
};

static Optional<SimplifiedSelectorForBucketing> is_roundabout_selector_bucketable_as_something_simpler(CSS::Selector::SimpleSelector const& simple_selector)
{
    if (simple_selector.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
        return {};

    if (simple_selector.pseudo_class().type != CSS::PseudoClass::Is
        && simple_selector.pseudo_class().type != CSS::PseudoClass::Where)
        return {};

    if (simple_selector.pseudo_class().argument_selector_list.size() != 1)
        return {};

    auto const& argument_selector = *simple_selector.pseudo_class().argument_selector_list.first();

    auto const& compound_selector = argument_selector.compound_selectors().last();
    if (compound_selector.simple_selectors.size() != 1)
        return {};

    auto const& inner_simple_selector = compound_selector.simple_selectors.first();
    if (inner_simple_selector.type == CSS::Selector::SimpleSelector::Type::Class
        || inner_simple_selector.type == CSS::Selector::SimpleSelector::Type::Id) {
        return SimplifiedSelectorForBucketing { inner_simple_selector.type, inner_simple_selector.name() };
    }

    if (inner_simple_selector.type == CSS::Selector::SimpleSelector::Type::TagName) {
        return SimplifiedSelectorForBucketing { inner_simple_selector.type, inner_simple_selector.qualified_name().name.lowercase_name };
    }

    return {};
}

void StyleComputer::collect_selector_insights(Selector const& selector, SelectorInsights& insights)
{
    for (auto const& compound_selector : selector.compound_selectors()) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type == Selector::SimpleSelector::Type::PseudoClass) {
                if (simple_selector.pseudo_class().type == PseudoClass::Has) {
                    insights.has_has_selectors = true;
                }
                for (auto const& argument_selector : simple_selector.pseudo_class().argument_selector_list) {
                    collect_selector_insights(*argument_selector, insights);
                }
            }
        }
    }
}

void StyleComputer::make_rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, SelectorInsights& insights)
{
    Vector<MatchingRule> matching_rules;
    size_t style_sheet_index = 0;
    for_each_stylesheet(cascade_origin, [&](auto& sheet, GC::Ptr<DOM::ShadowRoot> shadow_root) {
        auto& rule_caches = [&] -> RuleCaches& {
            RuleCachesForDocumentAndShadowRoots* rule_caches_for_document_or_shadow_root = nullptr;
            switch (cascade_origin) {
            case CascadeOrigin::Author:
                rule_caches_for_document_or_shadow_root = m_author_rule_cache;
                break;
            case CascadeOrigin::User:
                rule_caches_for_document_or_shadow_root = m_user_rule_cache;
                break;
            case CascadeOrigin::UserAgent:
                rule_caches_for_document_or_shadow_root = m_user_agent_rule_cache;
                break;
            default:
                VERIFY_NOT_REACHED();
            }
            if (!shadow_root)
                return rule_caches_for_document_or_shadow_root->for_document;
            return *rule_caches_for_document_or_shadow_root->for_shadow_roots.ensure(*shadow_root, [] { return make<RuleCaches>(); });
        }();

        size_t rule_index = 0;
        sheet.for_each_effective_style_producing_rule([&](auto const& rule) {
            SelectorList const& absolutized_selectors = [&]() {
                if (rule.type() == CSSRule::Type::Style)
                    return static_cast<CSSStyleRule const&>(rule).absolutized_selectors();
                if (rule.type() == CSSRule::Type::NestedDeclarations)
                    return static_cast<CSSNestedDeclarations const&>(rule).parent_style_rule().absolutized_selectors();
                VERIFY_NOT_REACHED();
            }();

            for (auto const& selector : absolutized_selectors) {
                m_style_invalidation_data->build_invalidation_sets_for_selector(selector);
            }

            for (CSS::Selector const& selector : absolutized_selectors) {
                MatchingRule matching_rule {
                    shadow_root,
                    &rule,
                    sheet,
                    sheet.default_namespace(),
                    selector,
                    style_sheet_index,
                    rule_index,
                    selector.specificity(),
                    cascade_origin,
                    false,
                };

                auto const& qualified_layer_name = matching_rule.qualified_layer_name();
                auto& rule_cache = qualified_layer_name.is_empty() ? rule_caches.main : *rule_caches.by_layer.ensure(qualified_layer_name, [] { return make<RuleCache>(); });

                bool contains_root_pseudo_class = false;
                Optional<CSS::PseudoElement> pseudo_element;

                collect_selector_insights(selector, insights);

                for (auto const& simple_selector : selector.compound_selectors().last().simple_selectors) {
                    if (!matching_rule.contains_pseudo_element) {
                        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoElement) {
                            matching_rule.contains_pseudo_element = true;
                            pseudo_element = simple_selector.pseudo_element().type();
                        }
                    }
                    if (!contains_root_pseudo_class) {
                        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoClass
                            && simple_selector.pseudo_class().type == CSS::PseudoClass::Root) {
                            contains_root_pseudo_class = true;
                        }
                    }
                }

                for (size_t i = 0; i < to_underlying(PseudoClass::__Count); ++i) {
                    auto pseudo_class = static_cast<PseudoClass>(i);
                    // If we're not building a rule cache for this pseudo class, just ignore it.
                    if (!m_pseudo_class_rule_cache[i])
                        continue;
                    if (selector.contains_pseudo_class(pseudo_class)) {
                        // For pseudo class rule caches we intentionally pass no pseudo-element, because we don't want to bucket pseudo class rules by pseudo-element type.
                        m_pseudo_class_rule_cache[i]->add_rule(matching_rule, {}, contains_root_pseudo_class);
                    }
                }

                rule_cache.add_rule(matching_rule, pseudo_element, contains_root_pseudo_class);
            }
            ++rule_index;
        });

        // Loosely based on https://drafts.csswg.org/css-animations-2/#keyframe-processing
        sheet.for_each_effective_keyframes_at_rule([&](CSSKeyframesRule const& rule) {
            auto keyframe_set = adopt_ref(*new Animations::KeyframeEffect::KeyFrameSet);
            HashTable<PropertyID> animated_properties;

            // Forwards pass, resolve all the user-specified keyframe properties.
            for (auto const& keyframe_rule : *rule.css_rules()) {
                auto const& keyframe = as<CSSKeyframeRule>(*keyframe_rule);
                Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame resolved_keyframe;

                auto key = static_cast<u64>(keyframe.key().value() * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);
                auto const& keyframe_style = *keyframe.style();
                for (auto const& it : keyframe_style.properties()) {
                    // Unresolved properties will be resolved in collect_animation_into()
                    for_each_property_expanding_shorthands(it.property_id, it.value, [&](PropertyID shorthand_id, CSSStyleValue const& shorthand_value) {
                        animated_properties.set(shorthand_id);
                        resolved_keyframe.properties.set(shorthand_id, NonnullRefPtr<CSSStyleValue const> { shorthand_value });
                    });
                }

                keyframe_set->keyframes_by_key.insert(key, resolved_keyframe);
            }

            Animations::KeyframeEffect::generate_initial_and_final_frames(keyframe_set, animated_properties);

            if constexpr (LIBWEB_CSS_DEBUG) {
                dbgln("Resolved keyframe set '{}' into {} keyframes:", rule.name(), keyframe_set->keyframes_by_key.size());
                for (auto it = keyframe_set->keyframes_by_key.begin(); it != keyframe_set->keyframes_by_key.end(); ++it)
                    dbgln("    - keyframe {}: {} properties", it.key(), it->properties.size());
            }

            rule_caches.main.rules_by_animation_keyframes.set(rule.name(), move(keyframe_set));
        });
        ++style_sheet_index;
    });
}

struct LayerNode {
    OrderedHashMap<FlyString, LayerNode> children {};
};

static void flatten_layer_names_tree(Vector<FlyString>& layer_names, StringView const& parent_qualified_name, FlyString const& name, LayerNode const& node)
{
    FlyString qualified_name = parent_qualified_name.is_empty() ? name : MUST(String::formatted("{}.{}", parent_qualified_name, name));

    for (auto const& item : node.children)
        flatten_layer_names_tree(layer_names, qualified_name, item.key, item.value);

    layer_names.append(qualified_name);
}

void StyleComputer::build_qualified_layer_names_cache()
{
    LayerNode root;

    auto insert_layer_name = [&](FlyString const& internal_qualified_name) {
        auto* node = &root;
        internal_qualified_name.bytes_as_string_view()
            .for_each_split_view('.', SplitBehavior::Nothing, [&](StringView part) {
                auto local_name = MUST(FlyString::from_utf8(part));
                node = &node->children.ensure(local_name);
            });
    };

    // Walk all style sheets, identifying when we first see a @layer name, and add its qualified name to the list.
    // TODO: Separate the light and shadow-dom layers.
    for_each_stylesheet(CascadeOrigin::Author, [&](auto& sheet, GC::Ptr<DOM::ShadowRoot>) {
        // NOTE: Postorder so that a @layer block is iterated after its children,
        // because we want those children to occur before it in the list.
        sheet.for_each_effective_rule(TraversalOrder::Postorder, [&](auto& rule) {
            switch (rule.type()) {
            case CSSRule::Type::Import:
                // TODO: Handle `layer(foo)` in import rules once we implement that.
                break;
            case CSSRule::Type::LayerBlock: {
                auto& layer_block = static_cast<CSSLayerBlockRule const&>(rule);
                insert_layer_name(layer_block.internal_qualified_name({}));
                break;
            }
            case CSSRule::Type::LayerStatement: {
                auto& layer_statement = static_cast<CSSLayerStatementRule const&>(rule);
                auto qualified_names = layer_statement.internal_qualified_name_list({});
                for (auto& name : qualified_names)
                    insert_layer_name(name);
                break;
            }

                // Ignore everything else
            case CSSRule::Type::Style:
            case CSSRule::Type::Media:
            case CSSRule::Type::FontFace:
            case CSSRule::Type::Keyframes:
            case CSSRule::Type::Keyframe:
            case CSSRule::Type::Margin:
            case CSSRule::Type::Namespace:
            case CSSRule::Type::NestedDeclarations:
            case CSSRule::Type::Page:
            case CSSRule::Type::Property:
            case CSSRule::Type::Supports:
                break;
            }
        });
    });

    // Now, produce a flat list of qualified names to use later
    m_qualified_layer_names_in_order.clear();
    flatten_layer_names_tree(m_qualified_layer_names_in_order, ""sv, {}, root);
}

void StyleComputer::build_rule_cache()
{
    m_author_rule_cache = make<RuleCachesForDocumentAndShadowRoots>();
    m_user_rule_cache = make<RuleCachesForDocumentAndShadowRoots>();
    m_user_agent_rule_cache = make<RuleCachesForDocumentAndShadowRoots>();

    m_selector_insights = make<SelectorInsights>();
    m_style_invalidation_data = make<StyleInvalidationData>();

    if (auto user_style_source = document().page().user_style(); user_style_source.has_value()) {
        m_user_style_sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(document()), user_style_source.value()));
    }

    build_qualified_layer_names_cache();

    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Hover)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Active)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Focus)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::FocusWithin)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::FocusVisible)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Target)] = make<RuleCache>();

    make_rule_cache_for_cascade_origin(CascadeOrigin::Author, *m_selector_insights);
    make_rule_cache_for_cascade_origin(CascadeOrigin::User, *m_selector_insights);
    make_rule_cache_for_cascade_origin(CascadeOrigin::UserAgent, *m_selector_insights);
}

void StyleComputer::invalidate_rule_cache()
{
    m_author_rule_cache = nullptr;

    // NOTE: We could be smarter about keeping the user rule cache, and style sheet.
    //       Currently we are re-parsing the user style sheet every time we build the caches,
    //       as it may have changed.
    m_user_rule_cache = nullptr;
    m_user_style_sheet = nullptr;

    // NOTE: It might not be necessary to throw away the UA rule cache.
    //       If we are sure that it's safe, we could keep it as an optimization.
    m_user_agent_rule_cache = nullptr;

    m_pseudo_class_rule_cache = {};
    m_style_invalidation_data = nullptr;
}

void StyleComputer::did_load_font(FlyString const&)
{
    document().invalidate_style(DOM::StyleInvalidationReason::CSSFontLoaded);
}

GC::Ptr<FontLoader> StyleComputer::load_font_face(ParsedFontFace const& font_face, Function<void(RefPtr<Gfx::Typeface const>)> on_load)
{
    if (font_face.sources().is_empty()) {
        if (on_load)
            on_load({});
        return {};
    }

    FontFaceKey key {
        .family_name = font_face.font_family(),
        .weight = font_face.weight().value_or(0),
        .slope = font_face.slope().value_or(0),
    };

    // FIXME: Pass the sources directly, so the font loader can make use of the format information, or load local fonts.
    Vector<URL> urls;
    for (auto const& source : font_face.sources()) {
        if (source.local_or_url.has<URL>())
            urls.append(source.local_or_url.get<URL>());
        // FIXME: Handle local()
    }

    if (urls.is_empty()) {
        if (on_load)
            on_load({});
        return {};
    }

    auto loader = heap().allocate<FontLoader>(*this, font_face.parent_style_sheet(), font_face.font_family(), font_face.unicode_ranges(), move(urls), move(on_load));
    auto& loader_ref = *loader;
    auto maybe_font_loaders_list = m_loaded_fonts.get(key);
    if (maybe_font_loaders_list.has_value()) {
        maybe_font_loaders_list->append(move(loader));
    } else {
        FontLoaderList loaders;
        loaders.append(loader);
        m_loaded_fonts.set(OwnFontFaceKey(key), move(loaders));
    }
    // Actual object owned by font loader list inside m_loaded_fonts, this isn't use-after-move/free
    return loader_ref;
}

void StyleComputer::load_fonts_from_sheet(CSSStyleSheet& sheet)
{
    for (auto const& rule : sheet.rules()) {
        if (!is<CSSFontFaceRule>(*rule))
            continue;
        auto const& font_face_rule = static_cast<CSSFontFaceRule const&>(*rule);
        if (!font_face_rule.is_valid())
            continue;
        if (auto font_loader = load_font_face(font_face_rule.font_face())) {
            sheet.add_associated_font_loader(*font_loader);
        }
    }
}

void StyleComputer::unload_fonts_from_sheet(CSSStyleSheet& sheet)
{
    for (auto& [_, font_loader_list] : m_loaded_fonts) {
        font_loader_list.remove_all_matching([&](auto& font_loader) {
            return sheet.has_associated_font_loader(*font_loader);
        });
    }
}

NonnullRefPtr<CSSStyleValue const> StyleComputer::compute_value_of_custom_property(DOM::AbstractElement abstract_element, FlyString const& name, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts)
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of ComputedProperties.
    auto& document = abstract_element.document();

    auto value = abstract_element.get_custom_property(name);
    if (!value || value->is_initial())
        return document.custom_property_initial_value(name);

    // Unset is the same as inherit for inherited properties, and by default all custom properties are inherited.
    // FIXME: Support non-inherited registered custom properties.
    if (value->is_inherit() || value->is_unset()) {
        if (!abstract_element.parent_element())
            return document.custom_property_initial_value(name);
        auto inherited_value = DOM::AbstractElement { const_cast<DOM::Element&>(*abstract_element.parent_element()) }.get_custom_property(name);
        if (!inherited_value)
            return document.custom_property_initial_value(name);
        return inherited_value.release_nonnull();
    }

    if (value->is_revert()) {
        // FIXME: Implement reverting custom properties.
    }
    if (value->is_revert_layer()) {
        // FIXME: Implement reverting custom properties.
    }

    if (!value->is_unresolved() || !value->as_unresolved().contains_arbitrary_substitution_function())
        return value.release_nonnull();

    auto& unresolved = value->as_unresolved();
    return Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams {}, abstract_element.element(), abstract_element.pseudo_element(), name, unresolved, guarded_contexts);
}

void StyleComputer::compute_custom_properties(ComputedProperties&, DOM::AbstractElement abstract_element) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of ComputedProperties.
    auto custom_properties = abstract_element.custom_properties();
    decltype(custom_properties) resolved_custom_properties;

    for (auto const& [name, style_property] : custom_properties) {
        resolved_custom_properties.set(name,
            StyleProperty {
                .important = style_property.important,
                .property_id = style_property.property_id,
                .value = compute_value_of_custom_property(abstract_element, name),
                .custom_name = style_property.custom_name,
            });
    }
    abstract_element.set_custom_properties(move(resolved_custom_properties));
}

void StyleComputer::compute_math_depth(ComputedProperties& style, DOM::Element const* element, Optional<CSS::PseudoElement> pseudo_element) const
{
    // https://w3c.github.io/mathml-core/#propdef-math-depth

    // First, ensure that the relevant CSS properties have been defaulted.
    // FIXME: This should be more sophisticated.
    compute_defaulted_property_value(style, element, CSS::PropertyID::MathDepth, pseudo_element);
    compute_defaulted_property_value(style, element, CSS::PropertyID::MathStyle, pseudo_element);

    auto inherited_math_depth = [&]() {
        if (!element || !element->parent_element())
            return InitialValues::math_depth();
        return element->parent_element()->computed_properties()->math_depth();
    };

    auto const& value = style.property(CSS::PropertyID::MathDepth);
    if (!value.is_math_depth()) {
        style.set_math_depth(inherited_math_depth());
        return;
    }
    auto const& math_depth = value.as_math_depth();

    auto resolve_integer = [&](CSSStyleValue const& integer_value) {
        if (integer_value.is_integer())
            return integer_value.as_integer().integer();
        if (integer_value.is_calculated())
            return integer_value.as_calculated().resolve_integer_deprecated({}).value();
        VERIFY_NOT_REACHED();
    };

    // The computed value of the math-depth value is determined as follows:
    // - If the specified value of math-depth is auto-add and the inherited value of math-style is compact
    //   then the computed value of math-depth of the element is its inherited value plus one.
    if (math_depth.is_auto_add() && style.property(CSS::PropertyID::MathStyle).to_keyword() == Keyword::Compact) {
        style.set_math_depth(inherited_math_depth() + 1);
        return;
    }
    // - If the specified value of math-depth is of the form add(<integer>) then the computed value of
    //   math-depth of the element is its inherited value plus the specified integer.
    if (math_depth.is_add()) {
        style.set_math_depth(inherited_math_depth() + resolve_integer(*math_depth.integer_value()));
        return;
    }
    // - If the specified value of math-depth is of the form <integer> then the computed value of math-depth
    //   of the element is the specified integer.
    if (math_depth.is_integer()) {
        style.set_math_depth(resolve_integer(*math_depth.integer_value()));
        return;
    }
    // - Otherwise, the computed value of math-depth of the element is the inherited one.
    style.set_math_depth(inherited_math_depth());
}

static void for_each_element_hash(DOM::Element const& element, auto callback)
{
    callback(element.local_name().ascii_case_insensitive_hash());
    if (element.id().has_value())
        callback(element.id().value().hash());
    for (auto const& class_ : element.class_names())
        callback(class_.hash());
    element.for_each_attribute([&](auto& attribute) {
        callback(attribute.lowercase_name().hash());
    });
}

void StyleComputer::reset_ancestor_filter()
{
    m_ancestor_filter->clear();
}

void StyleComputer::push_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter->increment(hash);
    });
}

void StyleComputer::pop_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter->decrement(hash);
    });
}

size_t StyleComputer::number_of_css_font_faces_with_loading_in_progress() const
{
    size_t count = 0;
    for (auto const& [_, loaders] : m_loaded_fonts) {
        for (auto const& loader : loaders) {
            if (loader->is_loading())
                ++count;
        }
    }
    return count;
}

bool StyleComputer::may_have_has_selectors() const
{
    if (!has_valid_rule_cache())
        return true;

    build_rule_cache_if_needed();
    return m_selector_insights->has_has_selectors;
}

bool StyleComputer::have_has_selectors() const
{
    build_rule_cache_if_needed();
    return m_selector_insights->has_has_selectors;
}

void RuleCache::add_rule(MatchingRule const& matching_rule, Optional<PseudoElement> pseudo_element, bool contains_root_pseudo_class)
{
    // NOTE: We traverse the simple selectors in reverse order to make sure that class/ID buckets are preferred over tag buckets
    //       in the common case of div.foo or div#foo selectors.
    auto add_to_id_bucket = [&](FlyString const& name) {
        rules_by_id.ensure(name).append(matching_rule);
    };

    auto add_to_class_bucket = [&](FlyString const& name) {
        rules_by_class.ensure(name).append(matching_rule);
    };

    auto add_to_tag_name_bucket = [&](FlyString const& name) {
        rules_by_tag_name.ensure(name).append(matching_rule);
    };

    for (auto const& simple_selector : matching_rule.selector.compound_selectors().last().simple_selectors.in_reverse()) {
        if (simple_selector.type == Selector::SimpleSelector::Type::Id) {
            add_to_id_bucket(simple_selector.name());
            return;
        }
        if (simple_selector.type == Selector::SimpleSelector::Type::Class) {
            add_to_class_bucket(simple_selector.name());
            return;
        }
        if (simple_selector.type == Selector::SimpleSelector::Type::TagName) {
            add_to_tag_name_bucket(simple_selector.qualified_name().name.lowercase_name);
            return;
        }
        // NOTE: Selectors like `:is/where(.foo)` and `:is/where(.foo .bar)` are bucketed as class selectors for `foo` and `bar` respectively.
        if (auto simplified = is_roundabout_selector_bucketable_as_something_simpler(simple_selector); simplified.has_value()) {
            if (simplified->type == Selector::SimpleSelector::Type::TagName) {
                add_to_tag_name_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Class) {
                add_to_class_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Id) {
                add_to_id_bucket(simplified->name);
                return;
            }
        }
    }

    if (matching_rule.contains_pseudo_element && pseudo_element.has_value()) {
        if (Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            rules_by_pseudo_element[to_underlying(pseudo_element.value())].append(matching_rule);
        } else {
            // NOTE: We don't cache rules for unknown pseudo-elements. They can't match anything anyway.
        }
    } else if (contains_root_pseudo_class) {
        root_rules.append(matching_rule);
    } else {
        for (auto const& simple_selector : matching_rule.selector.compound_selectors().last().simple_selectors) {
            if (simple_selector.type == Selector::SimpleSelector::Type::Attribute) {
                rules_by_attribute_name.ensure(simple_selector.attribute().qualified_name.name.lowercase_name).append(matching_rule);
                return;
            }
        }
        other_rules.append(matching_rule);
    }
}

void RuleCache::for_each_matching_rules(DOM::Element const& element, Optional<PseudoElement> pseudo_element, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const
{
    for (auto const& class_name : element.class_names()) {
        if (auto it = rules_by_class.find(class_name); it != rules_by_class.end()) {
            if (callback(it->value) == IterationDecision::Break)
                return;
        }
    }
    if (auto id = element.id(); id.has_value()) {
        if (auto it = rules_by_id.find(id.value()); it != rules_by_id.end()) {
            if (callback(it->value) == IterationDecision::Break)
                return;
        }
    }
    if (auto it = rules_by_tag_name.find(element.lowercased_local_name()); it != rules_by_tag_name.end()) {
        if (callback(it->value) == IterationDecision::Break)
            return;
    }
    if (pseudo_element.has_value()) {
        if (Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            if (callback(rules_by_pseudo_element.at(to_underlying(pseudo_element.value()))) == IterationDecision::Break)
                return;
        } else {
            // NOTE: We don't cache rules for unknown pseudo-elements. They can't match anything anyway.
        }
    }

    if (element.is_document_element()) {
        if (callback(root_rules) == IterationDecision::Break)
            return;
    }

    IterationDecision decision = IterationDecision::Continue;
    element.for_each_attribute([&](auto& name, auto&) {
        if (auto it = rules_by_attribute_name.find(name); it != rules_by_attribute_name.end()) {
            decision = callback(it->value);
        }
    });
    if (decision == IterationDecision::Break)
        return;

    (void)callback(other_rules);
}

Length::FontMetrics const& StyleComputer::root_element_font_metrics_for_element(GC::Ptr<DOM::Element const> element) const
{
    if (element && element->document().document_element() == element)
        return m_default_font_metrics;
    return m_root_element_font_metrics;
}

}
