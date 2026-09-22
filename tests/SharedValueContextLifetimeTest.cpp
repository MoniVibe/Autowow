/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AiObjectContext.h"
#include "SharedValueContext.h"

#include "gtest/gtest.h"

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
