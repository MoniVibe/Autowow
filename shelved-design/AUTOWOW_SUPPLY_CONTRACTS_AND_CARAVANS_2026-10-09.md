# Supply contracts and caravans: tested first source slice

Guild representatives can express useful item/profile demand with a captured supplier, quantity, price and deadline.
The prepared contract policy reserves real copper, registers exact native delivery intents and pays only accepted
received quantities. Partial fulfillment preserves the remaining funds and all unrelated reservations. Expiry and
cancellation cannot release money while cargo or a handoff is unresolved.

The caravan policy binds original physical item GUIDs/counts/metadata, owners, carrier and configured route/version.
It requires a verified origin and ordered native checkpoints. Halting/resuming retains custody; route arrival still
needs actual receiver handoff. Loss/return create no payment or replacement resources. No timer or teleport can
deliver cargo. Standing, PvP glory, escorts and interception remain separate later integrations.

Both policies are actual C++ source for the new unlinked module, not an activated server feature. Isolated GCC13/C++17
tests cover five contract groups and three caravan groups; existing BankPolicy tests passed independently. Preserved
RED checks caught missing implementation, an unregistered handoff and a missing origin-evidence guard. Full module,
database/native bank integration, actual routes/actors and live contracts remain unqualified.

Final source review passed without serious findings. Root staged seven selected paths into
`strategy-module`, preserved existing documentation preimages, and reviewed the exact final diff.
The resulting receipt binds 37 native paths; the supported-update source and current S125 runtime/config custody
remain unchanged. The reviewed plan stays immutable, with completion recorded in its separate execution ledger.

See [SUPPLY_CONTRACTS.md](../core-patches/offline-checkpoint-20261011/strategy-module/docs/SUPPLY_CONTRACTS.md)
for detailed finance, manifests and recovery.

No core/Playerbots gameplay source, live config, database, population, controls, Git, server restart or push changed.
Protected actor lifecycle, checked durable transactions and native custody adapters are prerequisites for enabling
these rules. The first caravan kernel requires a whole-manifest terminal disposition; mixed losses stay held.

The next living chain is useful demand → reserve funds → actual city craft/stock → lock original goods → observed
native travel → receiver custody → paid settlement → useful consumption/equip. A pure test pass is not that chain.
