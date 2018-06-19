/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/hardfork.hpp>
#include <fc/uint128.hpp>

namespace graphene { namespace chain {

share_type cut_fee(share_type a, uint16_t p)
{
   if( a == 0 || p == 0 )
      return 0;
   if( p == GRAPHENE_100_PERCENT )
      return a;

   fc::uint128 r(a.value);
   r *= p;
   r /= GRAPHENE_100_PERCENT;
   return r.to_uint64();
}

void account_balance_object::adjust_balance(const asset& delta)
{
   assert(delta.asset_id == asset_type);
   balance += delta.amount;
}

void account_statistics_object::process_fees(const account_object& a, database& d) const
{
   if( pending_fees > 0 || pending_vested_fees > 0 )
   {
      auto pay_out_fees = [&](const account_object& account, share_type core_fee_total, bool require_vesting)
      {
         // Check the referrer -- if he's no longer a member, pay to the lifetime referrer instead.
         // No need to check the registrar; registrars are required to be lifetime members.
         if( d.get_account_by_uid( account.referrer ).is_basic_account(d.head_block_time()) )
            d.modify(account, [](account_object& a) {
               a.referrer = a.lifetime_referrer;
            });

         share_type network_cut = cut_fee(core_fee_total, account.network_fee_percentage);
         assert( network_cut <= core_fee_total );

#ifndef NDEBUG
         const auto& props = d.get_global_properties();

         share_type reserveed = cut_fee(network_cut, props.parameters.reserve_percent_of_fee);
         share_type accumulated = network_cut - reserveed;
         assert( accumulated + reserveed == network_cut );
#endif
         share_type lifetime_cut = cut_fee(core_fee_total, account.lifetime_referrer_fee_percentage);
         share_type referral = core_fee_total - network_cut - lifetime_cut;

         d.modify(asset_dynamic_data_id_type()(d), [network_cut](asset_dynamic_data_object& d) {
            d.accumulated_fees += network_cut;
         });

         // Potential optimization: Skip some of this math and object lookups by special casing on the account type.
         // For example, if the account is a lifetime member, we can skip all this and just deposit the referral to
         // it directly.
         share_type referrer_cut = cut_fee(referral, account.referrer_rewards_percentage);
         share_type registrar_cut = referral - referrer_cut;

         d.deposit_cashback(d.get_account_by_uid(account.lifetime_referrer), lifetime_cut, require_vesting);
         d.deposit_cashback(d.get_account_by_uid(account.referrer), referrer_cut, require_vesting);
         d.deposit_cashback(d.get_account_by_uid(account.registrar), registrar_cut, require_vesting);

         assert( referrer_cut + registrar_cut + accumulated + reserveed + lifetime_cut == core_fee_total );
      };

      pay_out_fees(a, pending_fees, true);
      pay_out_fees(a, pending_vested_fees, false);

      d.modify(*this, [&](account_statistics_object& s) {
         s.lifetime_fees_paid += pending_fees + pending_vested_fees;
         s.pending_fees = 0;
         s.pending_vested_fees = 0;
      });
   }
}

void account_statistics_object::pay_fee( share_type core_fee, share_type cashback_vesting_threshold )
{
   if( core_fee > cashback_vesting_threshold )
      pending_fees += core_fee;
   else
      pending_vested_fees += core_fee;
}

std::pair<fc::uint128_t,share_type> account_statistics_object::compute_coin_seconds_earned(const uint64_t window, const fc::time_point_sec now)const
{
   fc::time_point_sec now_rounded( ( now.sec_since_epoch() / 60 ) * 60 );
   // check average coins and max coin-seconds
   share_type new_average_coins;
   fc::uint128_t max_coin_seconds;

   share_type effective_balance = core_balance + core_leased_in - core_leased_out;

   if( now_rounded <= average_coins_last_update )
      new_average_coins = average_coins;
   else
   {
      uint64_t delta_seconds = ( now_rounded - average_coins_last_update ).to_seconds();
      if( delta_seconds >= window )
         new_average_coins = effective_balance;
      else
      {
         uint64_t old_seconds = window - delta_seconds;

         fc::uint128_t old_coin_seconds = fc::uint128_t( average_coins.value ) * old_seconds;
         fc::uint128_t new_coin_seconds = fc::uint128_t( effective_balance.value ) * delta_seconds;

         max_coin_seconds = old_coin_seconds + new_coin_seconds;
         new_average_coins = ( max_coin_seconds / window ).to_uint64();
      }
   }
   // kill rounding issue
   max_coin_seconds = fc::uint128_t( new_average_coins.value ) * window;

   // check earned coin-seconds
   fc::uint128_t new_coin_seconds_earned;
   if( now_rounded <= coin_seconds_earned_last_update )
      new_coin_seconds_earned = coin_seconds_earned;
   else
   {
      int64_t delta_seconds = ( now_rounded - coin_seconds_earned_last_update ).to_seconds();

      fc::uint128_t delta_coin_seconds = effective_balance.value;
      delta_coin_seconds *= delta_seconds;

      new_coin_seconds_earned = coin_seconds_earned + delta_coin_seconds;
   }
   if( new_coin_seconds_earned > max_coin_seconds )
      new_coin_seconds_earned = max_coin_seconds;

   return std::make_pair( new_coin_seconds_earned, new_average_coins );
}

void account_statistics_object::update_coin_seconds_earned(const uint64_t window, const fc::time_point_sec now)
{
   fc::time_point_sec now_rounded( ( now.sec_since_epoch() / 60 ) * 60 );
   if( now_rounded <= coin_seconds_earned_last_update && now_rounded <= average_coins_last_update )
      return;
   const auto& result = compute_coin_seconds_earned( window, now_rounded );
   coin_seconds_earned = result.first;
   coin_seconds_earned_last_update = now_rounded;
   average_coins = result.second;
   average_coins_last_update = now_rounded;
}

void account_statistics_object::set_coin_seconds_earned(const fc::uint128_t new_coin_seconds, const fc::time_point_sec now)
{
   fc::time_point_sec now_rounded( ( now.sec_since_epoch() / 60 ) * 60 );
   coin_seconds_earned = new_coin_seconds;
   if( coin_seconds_earned_last_update < now_rounded )
      coin_seconds_earned_last_update = now_rounded;
}

set<account_uid_type> account_member_index::get_account_members(const account_object& a)const
{
   set<account_uid_type> result;
   for( auto auth : a.owner.account_uid_auths )
      result.insert(auth.first.uid);
   for( auto auth : a.active.account_uid_auths )
      result.insert(auth.first.uid);
   for( auto auth : a.secondary.account_uid_auths )
      result.insert(auth.first.uid);
   return result;
}
set<public_key_type> account_member_index::get_key_members(const account_object& a)const
{
   set<public_key_type> result;
   for( auto auth : a.owner.key_auths )
      result.insert(auth.first);
   for( auto auth : a.active.key_auths )
      result.insert(auth.first);
   result.insert( a.memo_key );
   return result;
}

void account_member_index::object_inserted(const object& obj)
{
    assert( dynamic_cast<const account_object*>(&obj) ); // for debug only
    const account_object& a = static_cast<const account_object&>(obj);

    auto account_members = get_account_members(a);
    for( auto item : account_members )
       account_to_account_memberships[item].insert(a.uid);

    auto key_members = get_key_members(a);
    for( auto item : key_members )
       account_to_key_memberships[item].insert(a.uid);
}

void account_member_index::object_removed(const object& obj)
{
    assert( dynamic_cast<const account_object*>(&obj) ); // for debug only
    const account_object& a = static_cast<const account_object&>(obj);

    auto key_members = get_key_members(a);
    for( auto item : key_members )
       account_to_key_memberships[item].erase( a.uid );

    auto account_members = get_account_members(a);
    for( auto item : account_members )
       account_to_account_memberships[item].erase( a.uid );
}

void account_member_index::about_to_modify(const object& before)
{
   before_key_members.clear();
   before_account_members.clear();
   assert( dynamic_cast<const account_object*>(&before) ); // for debug only
   const account_object& a = static_cast<const account_object&>(before);
   before_key_members     = get_key_members(a);
   before_account_members = get_account_members(a);
}

void account_member_index::object_modified(const object& after)
{
    assert( dynamic_cast<const account_object*>(&after) ); // for debug only
    const account_object& a = static_cast<const account_object&>(after);

    {
       set<account_uid_type> after_account_members = get_account_members(a);
       vector<account_uid_type> removed; removed.reserve(before_account_members.size());
       std::set_difference(before_account_members.begin(), before_account_members.end(),
                           after_account_members.begin(), after_account_members.end(),
                           std::inserter(removed, removed.end()));

       for( auto itr = removed.begin(); itr != removed.end(); ++itr )
          account_to_account_memberships[*itr].erase(a.uid);

       vector<account_uid_type> added; added.reserve(after_account_members.size());
       std::set_difference(after_account_members.begin(), after_account_members.end(),
                           before_account_members.begin(), before_account_members.end(),
                           std::inserter(added, added.end()));

       for( auto itr = added.begin(); itr != added.end(); ++itr )
          account_to_account_memberships[*itr].insert(a.uid);
    }


    {
       set<public_key_type> after_key_members = get_key_members(a);

       vector<public_key_type> removed; removed.reserve(before_key_members.size());
       std::set_difference(before_key_members.begin(), before_key_members.end(),
                           after_key_members.begin(), after_key_members.end(),
                           std::inserter(removed, removed.end()));

       for( auto itr = removed.begin(); itr != removed.end(); ++itr )
          account_to_key_memberships[*itr].erase(a.uid);

       vector<public_key_type> added; added.reserve(after_key_members.size());
       std::set_difference(after_key_members.begin(), after_key_members.end(),
                           before_key_members.begin(), before_key_members.end(),
                           std::inserter(added, added.end()));

       for( auto itr = added.begin(); itr != added.end(); ++itr )
          account_to_key_memberships[*itr].insert(a.uid);
    }
}

void account_referrer_index::object_inserted( const object& obj )
{
}
void account_referrer_index::object_removed( const object& obj )
{
}
void account_referrer_index::about_to_modify( const object& before )
{
}
void account_referrer_index::object_modified( const object& after  )
{
}

} } // graphene::chain
