#include "snax.system.hpp"
#include <snaxlib/dispatcher.hpp>

#include "producer_pay.cpp"
#include "delegate_bandwidth.cpp"
#include "voting.cpp"
#include "exchange_state.cpp"
#include <math.h>


namespace snaxsystem {

   system_contract::system_contract( account_name s )
   :native(s),
    _voters(_self,_self),
    _producers(_self,_self),
    _global(_self,_self),
    _rammarket(_self,_self)
   {
      //print( "construct system\n" );
      _gstate = _global.exists() ? _global.get() : get_default_parameters();

      auto itr = _rammarket.find(S(4,RAMCORE));

      if( itr == _rammarket.end() ) {
         auto system_token_supply   = snax::token(N(snax.token)).get_supply(snax::symbol_type(system_token_symbol).name()).amount;
         if( system_token_supply > 0 ) {
            itr = _rammarket.emplace( _self, [&]( auto& m ) {
               m.supply.amount = 100000000000000ll;
               m.supply.symbol = S(4,RAMCORE);
               m.base.balance.amount = int64_t(_gstate.free_ram());
               m.base.balance.symbol = S(0,RAM);
               m.quote.balance.amount = system_token_supply / 1000;
               m.quote.balance.symbol = CORE_SYMBOL;
            });
         }
      } else {
         //print( "ram market already created" );
      }

   }

   snax_global_state system_contract::get_default_parameters() {
      snax_global_state dp;
      get_blockchain_parameters(dp);
      return dp;
   }

   system_contract::~system_contract() {
      //print( "destruct system\n" );
      _global.set( _gstate, _self );
      //snax_exit(0);
   }

   void system_contract::emitplatform( account_name& platform ) {
        require_auth(platform);

        _platform_requests platform_requests(_self, platform);

        platform_config* found_config = nullptr;

        for (auto& config: _gstate.platforms) {
           if (!config.account) break;
           if (config.account == platform) {
               found_config = &config;
           }
        };

        snax_assert(found_config != nullptr, "platform not found in platforms config");

        const auto current_time = snax::time_point_sec(now());

        if (platform_requests.cbegin() != platform_requests.cend()) {
           auto last_request = platform_requests.end();
           snax_assert(
               (--last_request)->request
                    .to_time_point()
                    .time_since_epoch()
                    .to_seconds()
                    + 3600 * found_config -> period
               <=
               block_timestamp(current_time)
                    .to_time_point()
                    .time_since_epoch()
                    .to_seconds(),
               "platform can't request new amount of tokens because of period"
           );
        }

        const double minimal_supply_round_amount = static_cast<double>(_gstate.min_supply_points);
        snax_assert(_gstate.circulating_supply + asset(10000) <= _gstate.total_supply, "system emission stopped");
        double a, b, offset1, offset2;
        const double total_supply_amount = convert_asset_to_double(_gstate.total_supply);
        double offset = static_cast<double>(_gstate.supply_offset);

        std::tie(a, b) = get_parabola(minimal_supply_round_amount, total_supply_amount);

        const double current_offset = offset + 1;
        const double supply_limit_in_round =
                calc_parabola(a, b, total_supply_amount, offset) -
                (offset < minimal_supply_round_amount - 1 ?
                 calc_parabola(a, b, total_supply_amount, current_offset):
                 calc_parabola(a, b, total_supply_amount,
                               minimal_supply_round_amount));

        const asset round_supply = asset(static_cast<int64_t>(supply_limit_in_round));

        std::tie(offset1, offset2) = solve_quadratic_equation(a, b, static_cast<double>(_gstate.circulating_supply.amount));
        offset = offset1 < minimal_supply_round_amount ? offset1 : offset2;

        const auto amount_to_transfer = round_supply * static_cast<int64_t>(found_config->weight * 10000) / 10000;

        const auto amount_to_issue = amount_to_transfer.amount - get_balance().amount;

        if (amount_to_issue > 0) {
            INLINE_ACTION_SENDER(snax::token, issue)(
                N(snax.token), {N(snax),N(active)},
                {
                    N(snax),
                    asset(amount_to_issue),
                    "amount to issue to pay platform users"
                }
            );
            _gstate.circulating_supply += asset(amount_to_issue);
        }

        INLINE_ACTION_SENDER(snax::token, transfer)(
            N(snax.token), {N(snax),N(active)},
            {
                N(snax),
                platform,
                amount_to_transfer,
                "platform round supply"
            }
        );

        platform_requests.emplace(_self, [&](auto& record) {
            record.token_amount = amount_to_transfer;
            record.request = block_timestamp(current_time);
        });

        _global.set( _gstate, _self );

   }

   void system_contract::setram( uint64_t max_ram_size ) {
      require_auth( _self );

      snax_assert( _gstate.max_ram_size < max_ram_size, "ram may only be increased" ); /// decreasing ram might result market maker issues
      snax_assert( max_ram_size < 1024ll*1024*1024*1024*1024, "ram size is unrealistic" );
      snax_assert( max_ram_size > _gstate.total_ram_bytes_reserved, "attempt to set max below reserved" );

      auto delta = int64_t(max_ram_size) - int64_t(_gstate.max_ram_size);
      auto itr = _rammarket.find(S(4,RAMCORE));

      /**
       *  Increase or decrease the amount of ram for sale based upon the change in max
       *  ram size.
       */
      _rammarket.modify( itr, 0, [&]( auto& m ) {
         m.base.balance.amount += delta;
      });

      _gstate.max_ram_size = max_ram_size;
      _global.set( _gstate, _self );
   }

   void system_contract::setparams( const snax::blockchain_parameters& params ) {
      require_auth( N(snax) );
      (snax::blockchain_parameters&)(_gstate) = params;
      snax_assert( 3 <= _gstate.max_authority_depth, "max_authority_depth should be at least 3" );
      double total_weight = 0;
      for (auto& platform: params.platforms) {
          total_weight += platform.weight;
          snax_assert(platform.period > 0, "platform period must be greater than 0");
      }
      snax_assert(total_weight == 1 || total_weight == 0, "Summary weight of all platforms must be equal to 1 or 0");
      set_blockchain_parameters( params );
   }

   void system_contract::setpriv( account_name account, uint8_t ispriv ) {
      require_auth( _self );
      set_privileged( account, ispriv );
   }

   void system_contract::rmvproducer( account_name producer ) {
      require_auth( _self );
      auto prod = _producers.find( producer );
      snax_assert( prod != _producers.end(), "producer not found" );
      _producers.modify( prod, 0, [&](auto& p) {
            p.deactivate();
         });
   }

   void system_contract::bidname( account_name bidder, account_name newname, asset bid ) {
      require_auth( bidder );
      snax_assert( snax::name_suffix(newname) == newname, "you can only bid on top-level suffix" );
      snax_assert( newname != 0, "the empty name is not a valid account name to bid on" );
      snax_assert( (newname & 0xFull) == 0, "13 character names are not valid account names to bid on" );
      snax_assert( (newname & 0x1F0ull) == 0, "accounts with 12 character names and no dots can be created without bidding required" );
      snax_assert( !is_account( newname ), "account already exists" );
      snax_assert( bid.symbol == asset().symbol, "asset must be system token" );
      snax_assert( bid.amount > 0, "insufficient bid" );

      INLINE_ACTION_SENDER(snax::token, transfer)( N(snax.token), {bidder,N(active)},
                                                    { bidder, N(snax.names), bid, std::string("bid name ")+(name{newname}).to_string()  } );

      name_bid_table bids(_self,_self);
      print( name{bidder}, " bid ", bid, " on ", name{newname}, "\n" );
      auto current = bids.find( newname );
      if( current == bids.end() ) {
         bids.emplace( bidder, [&]( auto& b ) {
            b.newname = newname;
            b.high_bidder = bidder;
            b.high_bid = bid.amount;
            b.last_bid_time = current_time();
         });
      } else {
         snax_assert( current->high_bid > 0, "this auction has already closed" );
         snax_assert( bid.amount - current->high_bid > (current->high_bid / 10), "must increase bid by 10%" );
         snax_assert( current->high_bidder != bidder, "account is already highest bidder" );

         INLINE_ACTION_SENDER(snax::token, transfer)( N(snax.token), {N(snax.names),N(active)},
                                                       { N(snax.names), current->high_bidder, asset(current->high_bid),
                                                       std::string("refund bid on name ")+(name{newname}).to_string()  } );

         bids.modify( current, bidder, [&]( auto& b ) {
            b.high_bidder = bidder;
            b.high_bid = bid.amount;
            b.last_bid_time = current_time();
         });
      }
   }

   /**
    *  Called after a new account is created. This code enforces resource-limits rules
    *  for new accounts as well as new account naming conventions.
    *
    *  Account names containing '.' symbols must have a suffix equal to the name of the creator.
    *  This allows users who buy a premium name (shorter than 12 characters with no dots) to be the only ones
    *  who can create accounts with the creator's name as a suffix.
    *
    */
   void native::newaccount( account_name     creator,
                            account_name     newact
                            /*  no need to parse authorities
                            const authority& owner,
                            const authority& active*/ ) {

      if( creator != _self ) {
         auto tmp = newact >> 4;
         bool has_dot = false;

         for( uint32_t i = 0; i < 12; ++i ) {
           has_dot |= !(tmp & 0x1f);
           tmp >>= 5;
         }
         if( has_dot ) { // or is less than 12 characters
            auto suffix = snax::name_suffix(newact);
            if( suffix == newact ) {
               name_bid_table bids(_self,_self);
               auto current = bids.find( newact );
               snax_assert( current != bids.end(), "no active bid for name" );
               snax_assert( current->high_bidder == creator, "only highest bidder can claim" );
               snax_assert( current->high_bid < 0, "auction for name is not closed yet" );
               bids.erase( current );
            } else {
               snax_assert( creator == suffix, "only suffix may create this account" );
            }
         }
      }

      user_resources_table  userres( _self, newact);

      userres.emplace( newact, [&]( auto& res ) {
        res.owner = newact;
      });

      set_resource_limits( newact, 0, 0, 0 );
   }

    std::tuple<double, double>
    system_contract::solve_quadratic_equation(const double a, const double b, const double c) const {
        const double d = b * b - 4 * a * c;
        return std::make_tuple((-b + sqrt(d)) / 2 / a, (-b - sqrt(d)) / 2 / a);
    }

    std::tuple<double, double> system_contract::get_parabola(const double x0, const double y0) const {
        const double a = y0 / x0 / x0;
        return std::make_tuple(a, -2 * a * x0);
    }

    double system_contract::calc_parabola(const double a, const double b, const double c, const double x) const {
        return a * x * x + b * x + c;
    }

    double system_contract::convert_asset_to_double(const asset value) const {
        return static_cast<double>(value.amount);
    }

    asset system_contract::get_balance() {
        _accounts_balances balances(N(snax.token), _self);
        const auto& found = balances.find(S(4,CORE_SYMBOL));
        return found == balances.cend() ? asset(0): found->balance;
    }


} /// snax.system


SNAX_ABI( snaxsystem::system_contract,
     // native.hpp (newaccount definition is actually in snax.system.cpp)
     (newaccount)(updateauth)(deleteauth)(linkauth)(unlinkauth)(canceldelay)(onerror)
     // snax.system.cpp
     (emitplatform)(setram)(setparams)(setpriv)(rmvproducer)(bidname)
     // delegate_bandwidth.cpp
     (buyrambytes)(buyram)(sellram)(delegatebw)(undelegatebw)(refund)
     // voting.cpp
     (regproducer)(unregprod)(voteproducer)(regproxy)
     // producer_pay.cpp
     (onblock)(claimrewards)
)
