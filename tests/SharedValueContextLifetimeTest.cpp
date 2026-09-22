/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "SharedValueContext.h"

#include "gtest/gtest.h"

#include <vector>

TEST(SharedValueContextLifetime, GlobalValueIsOwnedAndReused)
{
    SharedValueContext& context = SharedValueContext::instance();
    Value<questGuidpMap>* first =
        context.getGlobalValue<questGuidpMap>("quest guidp map");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(context.GetCreated().count("quest guidp map"), 1u);
    first->Reset();

    Value<questGuidpMap>* second =
        context.getGlobalValue<questGuidpMap>("quest guidp map");
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(first, second);
    second->Reset();
}

namespace
{
class CountingSingleValue : public SingleCalculatedValue<std::vector<int>>
{
public:
    explicit CountingSingleValue(PlayerbotAI* botAI) : SingleCalculatedValue(botAI, "counting") {}
    int calculations = 0;

protected:
    std::vector<int> Calculate() override
    {
        ++calculations;
        return {calculations, 7, 11};
    }
};
}

// B1: RefGet on a compute-once value must not recompute (the inherited CalculatedValue::RefGet
// with checkInterval 1 would), must return the same content as the old by-value Get() path, and
// must hand out the same storage every time.
TEST(SharedValueContextLifetime, SingleCalculatedRefGetComputesOnceWithoutCopy)
{
    PlayerbotAI botAI;
    CountingSingleValue value(&botAI);

    std::vector<int> const& first = value.RefGet();
    for (int i = 0; i < 16; ++i)
        EXPECT_EQ(&value.RefGet(), &first);
    std::vector<int> const copied = value.Get();
    EXPECT_EQ(copied, first);
    EXPECT_EQ(value.calculations, 1);
    EXPECT_EQ(first, (std::vector<int>{1, 7, 11}));

    value.Reset();
    EXPECT_EQ(value.RefGet(), (std::vector<int>{2, 7, 11}));
    EXPECT_EQ(value.calculations, 2);
}
