/* Guarded exact one-off bot-to-bot guild trade through AzerothCore trade handlers. */
#ifndef MOD_PLAYERBOTS_AUTOWOW_GUILD_TRADE_CONTROL_H
#define MOD_PLAYERBOTS_AUTOWOW_GUILD_TRADE_CONTROL_H

#include <cstdint>
#include <string>

class Player;
class PlayerbotAI;

namespace AutoWowGuildTradeControl
{
inline constexpr std::uint32_t kMaximumPriceCopper = 500;

struct GuardFacts
{
    bool sellerOnline = false;
    bool buyerOnline = false;
    bool sellerPlayerbot = false;
    bool buyerPlayerbot = false;
    bool sellerAlive = false;
    bool buyerAlive = false;
    bool sellerInCombat = false;
    bool buyerInCombat = false;
    bool sameFaction = false;
    bool nearby = false;
    std::uint32_t sellerGuildId = 0;
    std::uint32_t buyerGuildId = 0;
    bool sellerAlreadyTrading = false;
    bool buyerAlreadyTrading = false;
    bool itemExists = false;
    bool itemEntryMatches = false;
    bool itemTradable = false;
    bool itemBoundToBuyer = false;
    std::uint32_t stackQuantity = 0;
    bool buyerCanStore = false;
    bool sellerCanReceiveMoney = false;
    std::uint32_t buyerMoney = 0;
};

// Empty means admitted; otherwise returns the stable fail-closed error suffix.
std::string GuardError(GuardFacts const& facts, std::uint32_t sellerGuid,
                       std::uint32_t buyerGuid, std::uint32_t itemGuid,
                       std::uint32_t itemEntry, std::uint32_t quantity,
                       std::uint32_t priceCopper);

struct ReceiptFacts
{
    bool itemAbsentFromSeller = false;
    bool exactItemOwnedByBuyer = false;
    std::uint32_t sellerMoneyBefore = 0;
    std::uint32_t sellerMoneyAfter = 0;
    std::uint32_t buyerMoneyBefore = 0;
    std::uint32_t buyerMoneyAfter = 0;
    std::uint32_t priceCopper = 0;
};

bool ReceiptComplete(ReceiptFacts const& facts);

std::string Start(Player* seller, PlayerbotAI* sellerAI, std::uint32_t buyerGuid,
                  std::uint32_t itemGuid, std::uint32_t itemEntry,
                  std::uint32_t quantity, std::uint32_t priceCopper);
std::string Status(std::uint32_t sellerGuid);
}

#endif
