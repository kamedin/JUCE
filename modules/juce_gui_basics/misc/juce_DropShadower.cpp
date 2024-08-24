/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

namespace juce
{

class DropShadower::ShadowWindow final : public Component
{
public:
    ShadowWindow (Component* comp, const DropShadow& ds)
        : target (comp), shadow (ds)
    {
        setVisible (true);
        setAccessible (false);
        setInterceptsMouseClicks (false, false);

        if (comp->isOnDesktop())
        {
           #if JUCE_WINDOWS
            const auto scope = [&]() -> std::unique_ptr<ScopedThreadDPIAwarenessSetter>
            {
                if (comp != nullptr)
                    if (auto* handle = comp->getWindowHandle())
                        return std::make_unique<ScopedThreadDPIAwarenessSetter> (handle);

                return nullptr;
            }();
           #endif

            setSize (1, 1); // to keep the OS happy by not having zero-size windows
            addToDesktop (ComponentPeer::windowIgnoresMouseClicks
                            | ComponentPeer::windowIsTemporary
                            | ComponentPeer::windowIgnoresKeyPresses
                         #if JUCE_WINDOWS
                            | ComponentPeer::windowIsOwned,
                          comp->getWindowHandle()
                         #endif
                         );
        }
        else if (Component* const parent = comp->getParentComponent())
        {
            parent->addChildComponent (this);
        }
    }

    void paint (Graphics& g) override
    {
        if (Component* c = target)
            shadow.drawForRectangle (g, getLocalArea (c, c->getLocalBounds()));
    }

    void resized() override
    {
        repaint();  // (needed for correct repainting)
    }

    float getDesktopScaleFactor() const override
    {
        if (target != nullptr)
            return target->getDesktopScaleFactor();

        return Component::getDesktopScaleFactor();
    }

private:
    WeakReference<Component> target;
    DropShadow shadow;

    JUCE_DECLARE_NON_COPYABLE (ShadowWindow)
};

class DropShadower::ComponentWithWeakReference
{
public:
    explicit ComponentWithWeakReference (Component& c)
        : ptr (&c), ref (&c) {}

    Component* get() const { return ref.get(); }

    bool operator< (const ComponentWithWeakReference& other) const { return ptr < other.ptr; }

private:
    Component* ptr;
    WeakReference<Component> ref;
};

//==============================================================================
DropShadower::DropShadower (const DropShadow& ds)  : shadow (ds)  {}

DropShadower::~DropShadower()
{
    for (auto& compEntry : observedComponents)
        if (auto* comp = compEntry.get())
            comp->removeComponentListener (this);

    const ScopedValueSetter<bool> setter (reentrant, true);
    shadowWindows.clear();
}

void DropShadower::setOwner (Component* componentToFollow)
{
    if (componentToFollow != owner)
    {
        // (the component can't be null)
        jassert (componentToFollow != nullptr);

        owner = componentToFollow;
        jassert (owner != nullptr);

        updateParentHierarchy();
    }
}

void DropShadower::componentMovedOrResized (Component& c, bool /*wasMoved*/, bool /*wasResized*/)
{
    if (owner == &c)
        updateShadows();
}

void DropShadower::componentBroughtToFront (Component& c)
{
    if (owner == &c)
        updateShadows();
}

void DropShadower::componentChildrenChanged (Component& c)
{
    if (owner != nullptr && owner->getParentComponent() == &c)
        updateShadows();
}

void DropShadower::componentParentHierarchyChanged (Component& c)
{
    if (owner == &c)
        updateParentHierarchy();
}

void DropShadower::componentVisibilityChanged (Component&)
{
    updateShadows();
}

void DropShadower::updateParentHierarchy()
{
    const auto lastSeenComponents = std::exchange (observedComponents, [&]
    {
        std::set<ComponentWithWeakReference> result;

        for (auto node = &*owner; node != nullptr; node = node->getParentComponent())
            result.emplace (*node);

        return result;
    }());

    const auto withDifference = [] (const auto& rangeA, const auto& rangeB, auto&& callback)
    {
        std::vector<ComponentWithWeakReference> result;
        std::set_difference (rangeA.begin(), rangeA.end(), rangeB.begin(), rangeB.end(), std::back_inserter (result));

        for (const auto& item : result)
            if (auto* c = item.get())
                callback (*c);
    };

    withDifference (lastSeenComponents, observedComponents, [this] (auto& comp) { comp.removeComponentListener (this); });
    withDifference (observedComponents, lastSeenComponents, [this] (auto& comp) { comp.addComponentListener (this); });

    updateShadows();
}

void DropShadower::updateShadows()
{
    if (reentrant)
        return;

    const ScopedValueSetter<bool> setter (reentrant, true);

    if (owner != nullptr
        && owner->isShowing()
        && owner->getWidth() > 0 && owner->getHeight() > 0
        && (Desktop::canUseSemiTransparentWindows() || owner->getParentComponent() != nullptr))
    {
        while (shadowWindows.size() < 4)
            shadowWindows.add (new ShadowWindow (owner, shadow));

        const int shadowEdge = jmax (shadow.offset.x, shadow.offset.y) + shadow.radius;
        const int x = owner->getX();
        const int y = owner->getY() - shadowEdge;
        const int w = owner->getWidth();
        const int h = owner->getHeight() + shadowEdge + shadowEdge;

        for (int i = 4; --i >= 0;)
        {
            // there seem to be rare situations where the dropshadower may be deleted by
            // callbacks during this loop, so use a weak ref to watch out for this..
            WeakReference<Component> sw (shadowWindows[i]);

            if (sw != nullptr)
            {
                sw->setAlwaysOnTop (owner->isAlwaysOnTop());

                if (sw == nullptr)
                    return;

                switch (i)
                {
                    case 0: sw->setBounds (x - shadowEdge, y, shadowEdge, h); break;
                    case 1: sw->setBounds (x + w, y, shadowEdge, h); break;
                    case 2: sw->setBounds (x, y, w, shadowEdge); break;
                    case 3: sw->setBounds (x, owner->getBottom(), w, shadowEdge); break;
                    default: break;
                }

                if (sw == nullptr)
                    return;

                sw->toBehind (i == 3 ? owner.get() : shadowWindows.getUnchecked (i + 1));
            }
        }
    }
    else
    {
        shadowWindows.clear();
    }
}

} // namespace juce
