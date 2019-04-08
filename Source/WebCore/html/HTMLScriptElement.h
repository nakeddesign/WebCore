/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Nikolas Zimmermann <zimmermann@kde.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#pragma once

#include "HTMLElement.h"
#include "ScriptElement.h"

namespace WebCore {

class HTMLScriptElement final : public HTMLElement, public ScriptElement {
public:
    static Ref<HTMLScriptElement> create(const QualifiedName&, Document&, bool wasInsertedByParser, bool alreadyStarted = false);

    String text() const { return scriptContent(); }
    WEBCORE_EXPORT void setText(const String&);

    URL src() const;

    WEBCORE_EXPORT void setAsync(bool);
    WEBCORE_EXPORT bool async() const;

    WEBCORE_EXPORT void setCrossOrigin(const AtomicString&);
    WEBCORE_EXPORT String crossOrigin() const;

private:
    HTMLScriptElement(const QualifiedName&, Document&, bool wasInsertedByParser, bool alreadyStarted);

    void parseAttribute(const QualifiedName&, const AtomicString&) final;
    InsertionNotificationRequest insertedInto(ContainerNode&) final;
    void finishedInsertingSubtree() final;
    void childrenChanged(const ChildChange&) final;

    bool isURLAttribute(const Attribute&) const final;

    void addSubresourceAttributeURLs(ListHashSet<URL>&) const final;

    String sourceAttributeValue() const final;
    String charsetAttributeValue() const final;
    String typeAttributeValue() const final;
    String languageAttributeValue() const final;
    String forAttributeValue() const final;
    String eventAttributeValue() const final;
    bool asyncAttributeValue() const final;
    bool deferAttributeValue() const final;
    bool hasSourceAttribute() const final;

    void dispatchLoadEvent() final;

    Ref<Element> cloneElementWithoutAttributesAndChildren(Document&) final;
};

} //namespace