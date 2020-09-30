#include "token.hpp"
#include "../utils/utils.cpp"

/**
   Creates a Cambiatus token.
   @author Julien Lucca
   @version 1.0

   Every token is related to a community. The community must exist in order for a token to be created.
   We use eosio::symbol type and check for the given params with the following rules:

   1) Currently supports two Token Types: `mcc` for multual credit clearing and `expiry` for expiration tokens
   2) Only the community issuer can create new Tokens
   3) Symbol must be unique and the same for both the community and the token
 */
void token::create(eosio::name issuer, eosio::asset max_supply,
                   eosio::asset min_balance, std::string type)
{
  auto sym = max_supply.symbol;
  eosio::check(max_supply.symbol == min_balance.symbol, "All assets must share the same symbol");
  eosio::check(type == "mcc" || type == "expiry", "type must be 'mcc' or 'expiry'");

  // Find existing community
  bespiral_communities communities(community_account, community_account.value);
  const auto &cmm = communities.get(sym.raw(), "can't find community. Cambiatus Tokens require a community.");

  eosio::check(sym.is_valid(), "invalid symbol");
  eosio::check(max_supply.is_valid(), "invalid max_supply");
  eosio::check(max_supply.amount > 0, "max max_supply must be positive");

  // Community creator must be the one creating the token
  require_auth(cmm.creator);

  // MCC only validations
  if (type == "mcc")
  {
    eosio::check(min_balance.is_valid(), "invalid min_balance");
    eosio::check(min_balance.amount <= 0, "min_balance must be equal or less than 0");
    eosio::check(max_supply.symbol == min_balance.symbol, "unmatched symbols for max_supply and min_balance. They must be the same");
  }

  stats statstable(_self, sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  eosio::check(existing == statstable.end(), "token with this symbol already exists");

  statstable.emplace(_self, [&](auto &s) {
    s.supply.symbol = max_supply.symbol;
    s.max_supply = max_supply;
    s.min_balance = min_balance;
    s.issuer = issuer;
    s.type = type;
  });

  // Notify creator
  require_recipient(cmm.creator);

  // Netlink issuer
  if (issuer != cmm.creator)
  {
    require_recipient(issuer);
    eosio::action netlink_issuer = eosio::action(eosio::permission_level{cmm.creator, eosio::name{"active"}}, // Permission
                                                 community_account,                                           // Account
                                                 eosio::name{"netlink"},                                      // Action
                                                 // cmm_asset, new_user, inviter
                                                 std::make_tuple(max_supply, issuer, cmm.creator));
    netlink_issuer.send();
  }

  // Create new balance for the creator
  accounts accounts(_self, issuer.value);
  accounts.emplace(_self, [&](auto &a) {
    a.balance = eosio::asset(0, max_supply.symbol);
    a.last_activity = now();
  });
}

/**
   Update token configurations
   @author Julien Lucca
   @version 1.0
*/
void token::update(eosio::asset max_supply, eosio::asset min_balance)
{
  eosio::check(max_supply.symbol == min_balance.symbol, "All assets must share the same symbol");

  eosio::check(min_balance.is_valid(), "invalid min_balance");
  eosio::check(max_supply.is_valid(), "invalid max_supply");
  eosio::check(max_supply.amount > 0, "max max_supply must be positive");

  // Find existing community
  bespiral_communities communities(community_account, community_account.value);
  const auto &cmm = communities.get(max_supply.symbol.raw(), "can't find community. Cambiatus Tokens require a community.");

  // Find token stats
  stats statstable(_self, max_supply.symbol.code().raw());
  const auto &st = statstable.get(min_balance.symbol.code().raw(), "token with given symbol does not exist, create token before issue");

  require_auth(st.issuer);

  statstable.modify(st, _self, [&](auto &s) {
    s.max_supply = max_supply;
    s.min_balance = min_balance;
  });
}

/**
   Issue / Mint tokens.
   @author Julien Lucca
   @version 1.0

   Allows the community to issue new tokens. It can be done by only by the issuer, and it is limited by the maximum supply available.

   You can choose to send the newly minted tokens to a specific account.
 */
void token::issue(eosio::name to, eosio::asset quantity, std::string memo)
{
  eosio::symbol sym = quantity.symbol;
  eosio::check(sym.is_valid(), "invalid symbol name");
  eosio::check(memo.size() <= 256, "memo has more than 256 bytes");

  stats statstable(get_self(), sym.code().raw());
  const auto &st = statstable.get(sym.code().raw(), "token with given symbol does not exist, create token before issue");

  // Require auth from the bespiral community contract
  require_auth(get_self());

  eosio::check(quantity.is_valid(), "invalid quantity");
  eosio::check(quantity.amount > 0, "must issue positive quantity");
  eosio::check(quantity.symbol == st.supply.symbol, "symbol mismatch");
  eosio::check(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

  statstable.modify(st, _self, [&](auto &s) {
    s.supply += quantity;
  });

  add_balance(st.issuer, quantity, st);

  if (to != st.issuer)
  {
    require_recipient(st.issuer);
    eosio::action transfer = eosio::action(eosio::permission_level{get_self(),eosio::name{"active"}},
					get_self(),
					eosio::name{"transfer"},
					std::make_tuple(st.issuer, to, quantity, memo));
    transfer.send();
  /* SEND_INLINE_ACTION(*this,
                       transfer,
                       {get_self(), eosio::name{"active"}},
                       {st.issuer, to, quantity, memo});
  */
  }

}

void token::transfer(eosio::name from, eosio::name to, eosio::asset quantity, std::string memo)
{
  eosio::check(from != to, "cannot transfer to self");

  // Require auth from self or from contract
  if (has_auth(from))
  {
    require_auth(from);
  }
  else
  {
    require_auth(get_self());
  }

  eosio::check(is_account(to), "destination account doesn't exists");

  // Find symbol stats
  auto sym = quantity.symbol;
  stats statstable(get_self(), sym.code().raw());
  const auto &st = statstable.get(sym.code().raw(), "token with given symbol doesn't exists");

  // Validate quantity and memo
  eosio::check(quantity.is_valid(), "invalid quantity");
  eosio::check(quantity.amount > 0, "quantity must be positive");
  eosio::check(quantity.symbol == st.max_supply.symbol, "symbol precision mismatch");
  eosio::check(memo.size() <= 256, "memo has more than 256 bytes");

  // Check if from belongs to the community
  bespiral_networks network(community_account, community_account.value);
  auto from_id = gen_uuid(quantity.symbol.raw(), from.value);
  auto itr_from = network.find(from_id);
  eosio::check(itr_from != network.end(), "from account doesn't belong to the community");

  // Check if to belongs to the community
  auto to_id = gen_uuid(quantity.symbol.raw(), to.value);
  auto itr_to = network.find(to_id);
  eosio::check(itr_to != network.end(), "to account doesn't belong to the community");

  // Transfer values
  sub_balance(from, quantity, st);
  add_balance(to, quantity, st);
}

/**
  Retire all tokens of a given currency
  @author Julien Lucca
  @version 1.0

  It can only be called and signed from the contract itself and it is used by the expiry feature.
  It removes all tokens out of the circulation
 */
// void token::retire(eosio::name from, eosio::asset quantity, std::string memo)
void token::retire(eosio::symbol currency, std::string user_type, std::string memo)
{
  require_auth(get_self());

  eosio::check(user_type == "natural" || user_type == "juridical", "User type must be 'natural' or 'juridical'");

  auto sym = currency;
  eosio::check(sym.is_valid(), "invalid symbol name");
  eosio::check(memo.size() <= 256, "memo has more than 256 bytes");

  token::stats statstable(_self, sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  eosio::check(existing != statstable.end(), "token with symbol does not exist");
  const auto &st = *existing;

  eosio::check(st.type == "expiry", "Cambiatus only retire tokens of the 'expiry' type");

  bespiral_networks network(community_account, community_account.value);
  auto network_by_cmm = network.get_index<eosio::name{"usersbycmm"}>();
  for (auto itr = network_by_cmm.find(currency.raw()); itr != network_by_cmm.end(); itr++)
  {
    // Make sure to retire only of a single type
    if (itr->user_type == user_type)
    {
      token::accounts accounts(_self, itr->invited_user.value);
      auto from_account = accounts.find(sym.code().raw());

      if (from_account != accounts.end())
      {
        // Decrease available supply
        statstable.modify(st, _self, [&](auto &s) {
          s.supply -= from_account->balance;
        });

        accounts.modify(from_account, _self, [&](auto &a) {
          a.balance = eosio::asset(0, currency);
          a.last_activity = now();
        });
      }
    }
  }
}

void token::initacc(eosio::symbol currency, eosio::name account, eosio::name inviter)
{
  // Validate auth -- can only be called by the Cambiatus contracts
  // require_auth(_self);
  if (eosio::get_sender() == community_account)
  {
    require_auth(inviter);
  }
  else
  {
    require_auth(_self);
  }

  // Make sure token exists on the stats table
  stats statstable(_self, currency.code().raw());
  const auto &st = statstable.get(currency.code().raw(), "token with given symbol does not exist, create token before initacc");

  // Make sure account belongs to the given community
  // Check if from belongs to the community
  bespiral_networks network(community_account, community_account.value);
  auto network_id = gen_uuid(currency.raw(), account.value);
  auto itr_net = network.find(network_id);
  eosio::check(itr_net != network.end(), "account doesn't belong to the community");

  // Create account table entry
  accounts accounts(_self, account.value);
  auto found_account = accounts.find(currency.code().raw());

  if (found_account == accounts.end())
  {
    accounts.emplace(_self, [&](auto &a) {
      a.balance = eosio::asset(0, st.supply.symbol);
      a.last_activity = now();
    });
  }
}

/**
 * Upsert Expiration options for a given currency.
 * @author Julien Lucca
 * @version 1.0
 *
 * Upsert expiration details on `expiryopts` table. Also fill amounts for every account on the network and schedules its retirement
 *
 * 1) Upserts given expiration options (`expiration_period` in seconds and `renovation_amount` in eosio::asset) for the given `currency`
 * 2) Iterates over the network table. For every account on the community.
 *  2.1) Issue for the account the given `renovation_amount`
 * 3) Schedules a `retire` action for the given `currency` after the given `expiration_period`
 */
void token::setexpiry(eosio::symbol currency, std::uint32_t natural_expiration_period, std::uint32_t juridical_expiration_period, eosio::asset renovation_amount)
{
  // Validate data
  eosio::check(currency.is_valid(), "invalid symbol name");

  // Validate community
  token::stats statstable(_self, currency.code().raw());
  auto existing = statstable.find(currency.code().raw());
  eosio::check(existing != statstable.end(), "token with symbol does not exist");
  const auto &st = *existing;

  eosio::check(st.type != "mcc", "you can only configure tokens of the 'expiry' type");
  eosio::check(currency == renovation_amount.symbol, "symbol precision mismatch");
  eosio::check(currency == st.supply.symbol, "symbol precision mismatch");

  // Only the token issuer can configure that
  require_auth(st.issuer);

  // Save data
  token::expiry_opts opts(_self, _self.value);
  auto old_opts = opts.find(currency.code().raw());

  if (old_opts == opts.end())
  {
    opts.emplace(_self, [&](auto &a) {
      a.currency = currency;
      a.natural_expiration_period = natural_expiration_period;
      a.juridical_expiration_period = juridical_expiration_period;
      a.renovation_amount = renovation_amount;
    });
  }
  else
  {
    opts.modify(old_opts, _self, [&](auto &a) {
      a.currency = currency;
      a.natural_expiration_period = natural_expiration_period;
      a.juridical_expiration_period = juridical_expiration_period;
      a.renovation_amount = renovation_amount;
    });
  }

  // Setup expiration
  bespiral_networks network(community_account, community_account.value);
  auto network_by_cmm = network.get_index<eosio::name{"usersbycmm"}>();
  for (auto itr = network_by_cmm.find(currency.raw()); itr != network_by_cmm.end(); itr++)
  {
    // Only natural users receive the
    if (itr->user_type == "natural")
    {

      std::string issue_memo = "Token Renewal, you received " +
                               renovation_amount.to_string() +
                               " tokens, valid for " +
                               std::to_string(natural_expiration_period) +
                               " seconds.";
      eosio::action issue = eosio::action(eosio::permission_level{get_self(), eosio::name{"active"}}, // Permission
                                          get_self(),                                                 // Account
                                          eosio::name{"issue"},                                       // Action
                                          std::make_tuple(itr->invited_user, renovation_amount, issue_memo));
      issue.send();
    }
  }

  auto natural_schedule_id = gen_uuid(currency.raw(), eosio::name{"natural"}.value);
  std::string natural_retire_memo = "Your tokens expired! Its been " + std::to_string(natural_expiration_period) + " seconds since the emission!";
  std::string natural_str = "natural";

  eosio::transaction retire_natural{};
  retire_natural.actions.emplace_back(eosio::permission_level{get_self(), eosio::name{"active"}}, // Permission
                                      get_self(),                                                 // Account
                                      eosio::name{"retire"},                                      // Action
                                      std::make_tuple(currency, natural_str, natural_retire_memo));
  retire_natural.delay_sec = natural_expiration_period;
  retire_natural.send(natural_schedule_id, get_self(), true);

  // Schedule retirement for juridical
  auto juridical_schedule_id = gen_uuid(currency.raw(), eosio::name{"juridical"}.value);
  std::string juridical_retire_memo = "Your tokens expired! Its been " + std::to_string(juridical_expiration_period) + " seconds since the emission!";
  std::string juridical_str = "juridical";

  eosio::transaction retire_juridical{};
  retire_juridical.actions.emplace_back(eosio::permission_level{get_self(), eosio::name{"active"}}, // Permission
                                        get_self(),                                                 // Account
                                        eosio::name{"retire"},                                      // Action
                                        std::make_tuple(currency, juridical_str, juridical_retire_memo));
  retire_juridical.delay_sec = juridical_expiration_period;
  retire_juridical.send(juridical_schedule_id, get_self(), true);
}

void token::sub_balance(eosio::name owner, eosio::asset value, const token::currency_stats &st)
{
  eosio::check(value.is_valid(), "Invalid value");
  eosio::check(value.amount > 0, "Can only transfer positive values");

  // Check for existing balance
  token::accounts accounts(_self, owner.value);
  auto from = accounts.find(value.symbol.code().raw());

  // Add balance table entry
  if (from == accounts.end())
  {
    eosio::check((value.amount * -1) >= st.min_balance.amount, "overdrawn community limit");

    accounts.emplace(_self, [&](auto &a) {
      a.balance = value;
      a.balance.amount *= -1;
      a.last_activity = now();
    });
  }
  else
  {
    auto new_balance = from->balance.amount - value.amount;
    eosio::check(new_balance >= st.min_balance.amount, "overdrawn community limit");
    accounts.modify(from, _self, [&](auto &a) {
      a.balance.amount -= value.amount;
      a.last_activity = now();
    });
  }
  return;
}

void token::add_balance(eosio::name recipient, eosio::asset value, const token::currency_stats &st)
{
  eosio::check(value.is_valid(), "Invalid value");
  eosio::check(value.amount > 0, "Can only transfer positive values");

  accounts accounts(_self, recipient.value);
  auto to = accounts.find(value.symbol.code().raw());

  if (to == accounts.end())
  {
    accounts.emplace(_self, [&](auto &a) {
      a.balance = value;
      a.last_activity = now();
    });
  }
  else
  {
    accounts.modify(to, _self, [&](auto &a) {
      a.balance += value;
      a.last_activity = now();
    });
  }
}

EOSIO_DISPATCH(token,
               (create)(update)(issue)(transfer)(retire)(setexpiry)(initacc));
