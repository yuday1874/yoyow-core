/*
 * Copyright (c) 2018, YOYOW Foundation PTE. LTD. and contributors.
 */
#include <graphene/chain/protocol/content.hpp>

#include "../utf8/checked.h"

namespace graphene { namespace chain {

void validate_platform_string( const string& str, const string& object_name = "", const int maxlen = GRAPHENE_MAX_PLATFORM_NAME_LENGTH )
{
   bool str_is_utf8 = true;
   bool str_too_long = false;
   uint32_t len = 0;
   uint32_t last_char = 0;

   auto itr = str.begin();
   while( itr != str.end() )
   {
      try
      {
         last_char = utf8::next( itr, str.end() );
         ++len;
         if( len > maxlen )
         {
            str_too_long = true;
            break;
         }
      }
      catch( utf8::invalid_utf8 e )
      {
         str_is_utf8 = false;
         break;
      }
   }

   FC_ASSERT( str_is_utf8, "platform ${o}should be in UTF-8", ("o", object_name) );
   FC_ASSERT( !str_too_long, "platform ${o}is too long", ("o", object_name)("length", len) );
}

void platform_create_operation::validate() const
{
   validate_op_fee( fee, "platform creation " );
   validate_account_uid( account, "platform " );
   validate_non_negative_core_asset( pledge, "pledge " );
   validate_platform_string( name, "name " );
   validate_platform_string( url, "url ", GRAPHENE_MAX_URL_LENGTH );
   validate_platform_string( extra_data, "extra_data ", GRAPHENE_MAX_PLATFORM_EXTRA_DATA_LENGTH );
}

share_type platform_create_operation::calculate_fee(const fee_parameters_type& k)const
{
   share_type core_fee_required = k.fee;
   core_fee_required += calculate_data_fee( fc::raw::pack_size(extra_data), k.price_per_kbyte );
   core_fee_required += calculate_data_fee( fc::raw::pack_size(name), k.price_per_kbyte );
   core_fee_required += calculate_data_fee( fc::raw::pack_size(url), k.price_per_kbyte );
   return core_fee_required;
}

void platform_update_operation::validate() const
{
   validate_op_fee( fee, "platform update " );
   validate_account_uid( account, "platform " );
   FC_ASSERT( new_pledge.valid() || new_name.valid() || new_url.valid() || new_extra_data.valid(), "Should change something" );
   if( new_pledge.valid() )
      validate_non_negative_core_asset( *new_pledge, "new pledge" );
   if( new_url.valid() )
      validate_platform_string( *new_url, "new url ", GRAPHENE_MAX_URL_LENGTH );
   if( new_name.valid() )
      validate_platform_string( *new_name, "new name " );
   if( new_extra_data.valid() )
      validate_platform_string( *new_extra_data, "new extra_data ", GRAPHENE_MAX_PLATFORM_EXTRA_DATA_LENGTH );
}

share_type platform_update_operation::calculate_fee(const fee_parameters_type& k)const
{
   share_type core_fee_required = k.fee;
   if( new_extra_data.valid() )
      core_fee_required += calculate_data_fee( fc::raw::pack_size(new_extra_data), k.price_per_kbyte );
   if( new_name.valid() )
      core_fee_required += calculate_data_fee( fc::raw::pack_size(new_name), k.price_per_kbyte );
   if( new_url.valid() )
      core_fee_required += calculate_data_fee( fc::raw::pack_size(new_url), k.price_per_kbyte );
   return core_fee_required;
}

void platform_vote_update_operation::validate()const
{
   validate_op_fee( fee, "platform vote update " );
   validate_account_uid( voter, "voter " );
   auto itr_add = platform_to_add.begin();
   auto itr_remove = platform_to_remove.begin();
   while( itr_add != platform_to_add.end() && itr_remove != platform_to_remove.end() )
   {
      FC_ASSERT( *itr_add != *itr_remove, "Can not add and remove same platform, uid: ${w}", ("w",*itr_add) );
      if( *itr_add < *itr_remove )
         ++itr_add;
      else
         ++itr_remove;
   }
   for( const auto uid : platform_to_add )
      validate_account_uid( uid, "platform " );
   for( const auto uid : platform_to_remove )
      validate_account_uid( uid, "platform " );
}

share_type platform_vote_update_operation::calculate_fee(const fee_parameters_type& k)const
{
   share_type core_fee_required = k.basic_fee;

   auto total_size = platform_to_add.size();
   core_fee_required += ( share_type( k.price_per_platform ) * total_size );

   return core_fee_required;
}

share_type post_operation::calculate_fee( const fee_parameters_type& schedule )const
{
   share_type core_fee_required = schedule.fee;
   auto hash_size = fc::raw::pack_size(hash_value);
   if( hash_size > 65 )
      core_fee_required += calculate_data_fee( hash_size, schedule.price_per_kbyte );
   core_fee_required += calculate_data_fee( fc::raw::pack_size(extra_data), schedule.price_per_kbyte );
   core_fee_required += calculate_data_fee( fc::raw::pack_size(title), schedule.price_per_kbyte );
   core_fee_required += calculate_data_fee( fc::raw::pack_size(body), schedule.price_per_kbyte );
   return core_fee_required;
}


void post_operation::validate()const
{
   validate_op_fee( fee, "post " );
   validate_account_uid( poster, "poster " );
   validate_account_uid( platform, "platform" );
   FC_ASSERT( post_pid > uint64_t( 0 ), "post_pid must be greater than 0 ");
   bool flag = origin_poster.valid() == origin_post_pid.valid();
   bool flag2 = origin_poster.valid() == origin_platform.valid();
   FC_ASSERT( flag && flag2, "origin poster and origin post pid and origin platform should be all presented or all not " );
   if( origin_poster.valid() )
      validate_account_uid( *origin_poster, "origin poster " );
   if( origin_platform.valid() )
      validate_account_uid( *origin_platform, "origin platform " );
   if( origin_post_pid.valid() )
      FC_ASSERT( *origin_post_pid > uint64_t( 0 ), "origin_post_pid must be greater than 0 ");
}

void post_update_operation::validate()const
{
   validate_op_fee( fee, "post update " );
   validate_account_uid( poster, "poster " );
   validate_account_uid( platform, "platform " );
   FC_ASSERT( post_pid > uint64_t( 0 ), "post_pid must be greater than 0 ");
}

share_type post_update_operation::calculate_fee( const fee_parameters_type& schedule )const
{
   share_type core_fee_required = schedule.fee;
   if( hash_value.valid() )
   {
      auto hash_size = fc::raw::pack_size(*hash_value);
      if( hash_size > 65 )
         core_fee_required += calculate_data_fee( hash_size, schedule.price_per_kbyte );
   }
   if( extra_data.valid() )
      core_fee_required += calculate_data_fee( fc::raw::pack_size(extra_data), schedule.price_per_kbyte );
   if( title.valid() )
      core_fee_required += calculate_data_fee( fc::raw::pack_size(title), schedule.price_per_kbyte );
   if( body.valid() )
      core_fee_required += calculate_data_fee( fc::raw::pack_size(body), schedule.price_per_kbyte );
   return core_fee_required;
}



} } // graphene::chain
