/*
* (C) 2024 Jack Lloyd
* (C) 2024 René Meusel - Rohde & Schwarz Cybersecurity
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include <botan/pk_options.h>

#include <botan/internal/fmt.h>
#include <botan/internal/scan_name.h>

namespace Botan {

PK_Signature_Options::PK_Signature_Options(std::string_view algo, std::string_view params, std::string_view provider) {
   /*
   * This is a convoluted mess because we must handle dispatch for every algorithm
   * specific detail of how padding strings were formatted in versions prior to 3.6.
   *
   * This will all go away once the deprecated constructors of PK_Signer and PK_Verifier
   * are removed in Botan4.
   */

   auto options = PK_Signature_Options();

   if(!provider.empty() && provider != "base") {
      with_provider(provider);
   }

   if(algo.starts_with("Dilithium") || algo == "SPHINCS+") {
      if(!params.empty() && params != "Randomized" && params != "Deterministic") {
         throw Invalid_Argument(fmt("Unexpected parameters for signing with {}", algo));
      }

      if(params == "Deterministic") {
         with_deterministic_signature();
      }
   } else if(algo == "SM2") {
      /*
      * SM2 parameters have the following possible formats:
      * Ident [since 2.2.0]
      * Ident,Hash [since 2.3.0]
      */
      if(params.empty()) {
         with_hash("SM3");
      } else {
         const auto [userid, hash] = [&]() -> std::pair<std::string_view, std::string_view> {
            if(const auto comma = params.find(','); comma != std::string::npos) {
               return {params.substr(0, comma), params.substr(comma + 1)};
            } else {
               return {params, "SM3"};
            }
         }();

         with_context(userid);
         with_hash(hash);
      }
   } else if(algo == "Ed25519") {
      if(!params.empty() && params != "Identity" && params != "Pure") {
         if(params == "Ed25519ph") {
            with_prehash();
         } else {
            with_prehash(std::string(params));
         }
      }
   } else if(algo == "Ed448") {
      if(!params.empty() && params != "Identity" && params != "Pure" && params != "Ed448") {
         if(params == "Ed448ph") {
            with_prehash();
         } else {
            with_prehash(std::string(params));
         }
      }
   } else if(algo == "RSA") {
      SCAN_Name req(params);

      // handling various deprecated aliases that have accumulated over the years ...
      auto padding = [](std::string_view alg) -> std::string_view {
         if(alg == "EMSA_PKCS1" || alg == "EMSA-PKCS1-v1_5" || alg == "EMSA3") {
            return "PKCS1v15";
         }

         if(alg == "PSSR_Raw") {
            return "PSS_Raw";
         }

         if(alg == "PSSR" || alg == "EMSA-PSS" || alg == "PSS-MGF1" || alg == "EMSA4") {
            return "PSS";
         }

         if(alg == "EMSA_X931" || alg == "EMSA2" || alg == "X9.31") {
            return "X9.31";
         }

         return alg;
      }(req.algo_name());

      if(padding == "Raw") {
         if(req.arg_count() == 0) {
            with_padding(padding);
         } else if(req.arg_count() == 1) {
            with_padding(padding).with_prehash(req.arg(0));
         } else {
            throw Invalid_Argument("Raw padding with more than one parameter");
         }
      } else if(padding == "PKCS1v15") {
         if(req.arg_count() == 2 && req.arg(0) == "Raw") {
            with_padding(padding);
            with_hash(req.arg(0));
            with_prehash(req.arg(1));
         } else if(req.arg_count() == 1) {
            with_padding(padding);
            with_hash(req.arg(0));
         } else {
            throw Lookup_Error("PKCS1v15 padding with unexpected parameters");
         }
      } else if(padding == "PSS_Raw" || padding == "PSS") {
         if(req.arg_count_between(1, 3) && req.arg(1, "MGF1") == "MGF1") {
            with_padding(padding);
            with_hash(req.arg(0));

            if(req.arg_count() == 3) {
               with_salt_size(req.arg_as_integer(2));
            }
         } else {
            throw Lookup_Error("PSS padding with unexpected parameters");
         }
      } else if(padding == "ISO_9796_DS2") {
         if(req.arg_count_between(1, 3)) {
            // FIXME
            const bool implicit = req.arg(1, "exp") == "imp";

            with_padding(padding);
            with_hash(req.arg(0));

            if(req.arg_count() == 3) {
               if(implicit) {
                  with_salt_size(req.arg_as_integer(2));
               } else {
                  with_salt_size(req.arg_as_integer(2));
                  with_explicit_trailer_field();
               }
            } else if(!implicit) {
               with_explicit_trailer_field();
            }
         } else {
            throw Lookup_Error("ISO-9796-2 DS2 padding with unexpected parameters");
         }
      } else if(padding == "ISO_9796_DS3") {
         //ISO-9796-2 DS 3 is deterministic and DS2 without a salt
         with_padding(padding);
         if(req.arg_count_between(1, 2)) {
            with_hash(req.arg(0));
            if(req.arg(1, "exp") != "imp") {
               with_explicit_trailer_field();
            }
         } else {
            throw Lookup_Error("ISO-9796-2 DS3 padding with unexpected parameters");
         }
      } else if(padding == "X9.31") {
         if(req.arg_count() == 1) {
            with_padding(padding);
            with_hash(req.arg(0));
         } else {
            throw Lookup_Error("X9.31 padding with unexpected parameters");
         }
      }
      // end of RSA block
   } else {
      if(!params.empty()) {
         // ECDSA/DSA/ECKCDSA/etc
         auto hash = [&]() {
            if(params.starts_with("EMSA1")) {
               SCAN_Name req(params);
               return req.arg(0);
            } else {
               return std::string(params);
            }
         };

         with_hash(hash());
      }
   }
}

void PK_Signature_Options::validate_for_hash_based_signature_algorithm(
   std::string_view algo_name, std::optional<std::string_view> acceptable_hash) {
   if(auto hash = take(m_hash_fn)) {
      if(!acceptable_hash.has_value()) {
         throw Invalid_Argument(fmt("This {} key does not support explicit hash function choice", algo_name));
      }

      if(hash != acceptable_hash.value()) {
         throw Invalid_Argument(
            fmt("This {} key can only be used with {}, not {}", algo_name, acceptable_hash.value(), hash.value()));
      }
   }
}

}  // namespace Botan
