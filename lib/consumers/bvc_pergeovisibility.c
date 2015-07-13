/*
 * This file is part of bgpstream
 *
 * CAIDA, UC San Diego
 * bgpstream-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 * Authors: Alistair King, Chiara Orsini
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "utils.h"
#include "libipmeta.h"
#include "khash.h"
#include "czmq.h"

#include "bgpstream_utils_pfx_set.h"
#include "bgpstream_utils_id_set.h"

#include "bgpview_consumer_interface.h"

#include "bvc_pergeovisibility.h"

#define BUFFER_LEN 1024

#define NAME                       "per-geo-visibility"
#define CONSUMER_METRIC_PREFIX     "prefix-visibility.geo.netacuity"


#define METRIC_PREFIX_FORMAT       "%s."CONSUMER_METRIC_PREFIX".%s.%s.v%d.%s"
#define METRIC_PREFIX_TH_FORMAT    "%s."CONSUMER_METRIC_PREFIX".%s.%s.v%d.visibility_threshold.%s.%s"
#define META_METRIC_PREFIX_FORMAT  "%s.meta.bgpview.consumer."NAME".%s"


/* #define METRIC_PREFIX_FORMAT       "%s.%s.%s.%s.v4.%s" */
/* #define METRIC_PREFIX_PERC_FORMAT  "%s.%s.%s.%s.v4.perc.%s.%s" */
/* #define META_METRIC_PREFIX_FORMAT  "%s.meta.bgpview.consumer."NAME".%s" */


#define GEO_PROVIDER_NAME  "netacq-edge"





#define STATE					\
  (BVC_GET_STATE(consumer, pergeovisibility))

#define CHAIN_STATE                             \
  (BVC_GET_CHAIN_STATE(consumer))


/* our 'class' */
static bvc_t bvc_pergeovisibility = {
  BVC_ID_PERGEOVISIBILITY,
  NAME,
  BVC_GENERATE_PTRS(pergeovisibility)
};


typedef enum {
  VIS_1_FF_ASN = 0,
  VIS_25_PERCENT = 1,
  VIS_50_PERCENT = 2,
  VIS_75_PERCENT = 3,
  VIS_100_PERCENT = 4,
} vis_thresholds_t;

#define VIS_THRESHOLDS_CNT 5

typedef struct visibility_counters {
  uint32_t visible_pfxs;
  uint64_t visible_ips;
  uint32_t ff_peer_asns_sum;  
} visibility_counters_t;


/** pergeo_info_t
 *  network visibility information related to a single
 *  geographical location (currently country codes)
 */
typedef struct pergeo_info {

  /** All v4 prefixes that this CC observed */
  bgpstream_ipv4_pfx_set_t *v4pfxs;

  /** All origin ASns this CC observed */
  bgpstream_id_set_t *asns;
  uint32_t asns_idx;

  /** number of visible prefixes based on 
   * thresholds (1 ff, or 25, 50, 75, 100 percent) */
  visibility_counters_t visibility_counters[VIS_THRESHOLDS_CNT];

  uint32_t visible_pfxs_idx[VIS_THRESHOLDS_CNT];  
  uint32_t visible_ips_idx[VIS_THRESHOLDS_CNT];
  uint32_t ff_peer_asns_sum_idx[VIS_THRESHOLDS_CNT];  

} pergeo_info_t;


static
char *threshold_string(int i)
{
  switch(i)
    {
    case VIS_1_FF_ASN:
      return "min_1_ff_peer_asn";
    case VIS_25_PERCENT:
      return "min_25%_ff_peer_asns";
    case VIS_50_PERCENT:
      return "min_50%_ff_peer_asns";
    case VIS_75_PERCENT:
      return "min_75%_ff_peer_asns";
    case VIS_100_PERCENT:
      return "min_100%_ff_peer_asns";
    default:
      return "ERROR";
    }
  return "ERROR";
}


static void
update_visibility_counters(visibility_counters_t *visibility_counters, uint8_t net_size,
                           int asns_count, int vX_ff)
{
  double ratio;
  uint64_t ips = 1 << net_size;
  if(vX_ff == 0 || asns_count <= 0)
    {
      return;
    }

  visibility_counters[VIS_1_FF_ASN].visible_pfxs++;
  visibility_counters[VIS_1_FF_ASN].visible_ips += ips;
  visibility_counters[VIS_1_FF_ASN].ff_peer_asns_sum += asns_count;

  ratio = (double) asns_count / (double) vX_ff;
  
  if(ratio == 1)
    {
      visibility_counters[VIS_100_PERCENT].visible_pfxs++;
      visibility_counters[VIS_100_PERCENT].visible_ips += ips;
      visibility_counters[VIS_100_PERCENT].ff_peer_asns_sum += asns_count;
    }
  if(ratio >= 0.75)
    {
      visibility_counters[VIS_75_PERCENT].visible_pfxs++;
      visibility_counters[VIS_75_PERCENT].visible_ips += ips;
      visibility_counters[VIS_75_PERCENT].ff_peer_asns_sum += asns_count;
    }
  if(ratio >= 0.5)
    {
      visibility_counters[VIS_50_PERCENT].visible_pfxs++;
      visibility_counters[VIS_50_PERCENT].visible_ips += ips;
      visibility_counters[VIS_50_PERCENT].ff_peer_asns_sum += asns_count;
    }
  if(ratio >= 0.25)
    {
      visibility_counters[VIS_25_PERCENT].visible_pfxs++;
      visibility_counters[VIS_25_PERCENT].visible_ips += ips;
      visibility_counters[VIS_25_PERCENT].ff_peer_asns_sum += asns_count;
    }                  
}

/** Destroy pergeo information
 *  @param info structure to destroy
 */
static void pergeo_info_destroy(pergeo_info_t info)
{
  if(info.v4pfxs != NULL)
    {
      bgpstream_ipv4_pfx_set_destroy(info.v4pfxs);
    }
  if(info.asns != NULL)
    {
      bgpstream_id_set_destroy(info.asns);
    }
}


/** Map from COUNTRYCODE => per geo info,
 *    i.e. a PFX-SET
 */
KHASH_INIT(cc_pfxs,
	   char *,
	   pergeo_info_t,
	   1,
	   kh_str_hash_func,
	   kh_str_hash_equal);


/** key package ids related to
 *  generic metrics
 */
typedef struct gen_metrics {

  int cache_misses_cnt_idx;
  int cache_hits_cnt_idx;
  int arrival_delay_idx;
  int processed_delay_idx;
  int processing_time_idx;
  int max_numcountries_perpfx_idx;
  double avg_numcountries_perpfx_idx;
  int num_visible_pfx_idx;
  int max_records_perpfx_idx;

} gen_metrics_t;


/* our 'instance' */
typedef struct bvc_pergeovisibility_state {
  int cache_misses_cnt;
  int cache_hits_cnt;
  int arrival_delay;
  int processed_delay;
  int processing_time;
  int max_numcountries_perpfx;
  double avg_numcountries_perpfx;
  int num_visible_pfx;
  int max_records_perpfx;

  /** Map from CC => pfxs-generated info */
  khash_t(cc_pfxs) *countrycode_pfxs;

  /** netacq-edge files */
  char blocks_file[BUFFER_LEN];
  char locations_file[BUFFER_LEN];
  char countries_file[BUFFER_LEN];

  /** Timeseries Key Package (gen) */
  timeseries_kp_t *kp_gen;

  /** Timeseries Key Package (v4) */
  timeseries_kp_t *kp_v4;

  /** General metric indexes */
  gen_metrics_t gen_metrics;

  /** ipmeta structures */
  ipmeta_t *ipmeta;
  ipmeta_provider_t *provider;
  ipmeta_record_set_t *records;

} bvc_pergeovisibility_state_t;


/** Print usage information to stderr */
static void usage(bvc_t *consumer)
{
  fprintf(stderr,
	  "consumer usage: %s\n"
	  "       -c <file>     country decode file (mandatory option)\n"
	  "       -b <file>     blocks file (mandatory option)\n"
	  "       -l <file>     locations file (mandatory option)\n",
	  consumer->name);
}


/** Parse the arguments given to the consumer */
static int parse_args(bvc_t *consumer, int argc, char **argv)
{
  int opt;

  assert(argc > 0 && argv != NULL);

  /* NB: remember to reset optind to 1 before using getopt! */
  optind = 1;

  /* remember the argv strings DO NOT belong to us */
  while((opt = getopt(argc, argv, ":b:c:l:?")) >= 0)
    {
      switch(opt)
	{
	case 'b':
	  strcpy(STATE->blocks_file, optarg);
	  break;
	case 'c':
	  strcpy(STATE->countries_file, optarg);
	  break;
	case 'l':
	  strcpy(STATE->locations_file, optarg);
	  break;
	case '?':
	case ':':
	default:
	  usage(consumer);
	  return -1;
	}
    }

  // blocks countries and locations are mandatory options
  if(STATE->blocks_file[0] == '\0' ||
     STATE->countries_file[0] ==  '\0' ||
     STATE->locations_file[0] == '\0')
    {
      usage(consumer);
      return -1;
    }


  return 0;
}

static int init_ipmeta(bvc_t *consumer)
{
  char provider_options[BUFFER_LEN] = "";

  /* lookup the provider using the name  */
  if((STATE->provider =
      ipmeta_get_provider_by_name(STATE->ipmeta, GEO_PROVIDER_NAME)) == NULL)
    {
      fprintf(stderr, "ERROR: Invalid provider name: %s\n", GEO_PROVIDER_NAME);
      return -1;
    }

  /* enable the provider  */
  snprintf(provider_options, BUFFER_LEN, "-b %s -l %s -c %s -D intervaltree",
	   STATE->blocks_file,
	   STATE->locations_file,
	   STATE->countries_file);

  if(ipmeta_enable_provider(STATE->ipmeta,
			    STATE->provider,
			    provider_options,
			    IPMETA_PROVIDER_DEFAULT_YES) != 0)
    {
      fprintf(stderr, "ERROR: Could not enable provider %s\n",
              GEO_PROVIDER_NAME);
      return -1;
    }

  /* initialize a (reusable) record set structure  */
  if((STATE->records = ipmeta_record_set_init()) == NULL)
    {
      fprintf(stderr, "ERROR: Could not init record set\n");
      return -1;
    }

  return 0;
}

static int create_per_cc_metrics(bvc_t *consumer)
{
  ipmeta_provider_netacq_edge_country_t **countries = NULL;
  int num_countries =
    ipmeta_provider_netacq_edge_get_countries(STATE->provider, &countries);
  int i;
  khiter_t k;
  int khret;
  pergeo_info_t geo_info;
  char buffer[BUFFER_LEN];

  int j;
  
  for(i=0; i < num_countries; i++)
    {
      // Warning: we assume netacq returns a set of unique countries
      // then we don't need to check if these iso2 are already
      // present in the countrycode map
      k = kh_put(cc_pfxs, STATE->countrycode_pfxs,
		 strdup(countries[i]->iso2), &khret);

      // initialize properly geo_info and create ipv4 metrics id for kp
      geo_info.v4pfxs = bgpstream_ipv4_pfx_set_create();
      if(geo_info.v4pfxs == NULL)
        {
	  fprintf(stderr, "ERROR: Could not create pfx set\n");
        }

      geo_info.asns = bgpstream_id_set_create();
      if(geo_info.asns == NULL)
        {
	  fprintf(stderr, "ERROR: Could not create asns set\n");
        }
            
      snprintf(buffer, BUFFER_LEN, METRIC_PREFIX_FORMAT,
               CHAIN_STATE->metric_prefix, 
               countries[i]->continent, countries[i]->iso2, bgpstream_ipv2number(BGPSTREAM_ADDR_VERSION_IPV4),
               "origin_asns_cnt");
      if((geo_info.asns_idx = timeseries_kp_add_key(STATE->kp_v4, buffer)) == -1)
	{
	  fprintf(stderr, "ERROR: Could not create key metric\n");
	}


      for(j=0; j<VIS_THRESHOLDS_CNT; j++)
        {
          geo_info.visibility_counters[j].visible_pfxs = 0;
          snprintf(buffer, BUFFER_LEN, METRIC_PREFIX_TH_FORMAT,
                   CHAIN_STATE->metric_prefix, countries[i]->continent, countries[i]->iso2,
                   bgpstream_ipv2number(BGPSTREAM_ADDR_VERSION_IPV4),
                   threshold_string(j), "visible_prefixes_cnt");             
          if((geo_info.visible_pfxs_idx[j] = timeseries_kp_add_key(STATE->kp_v4, buffer)) == -1)
            {
              fprintf(stderr, "ERROR: Could not create key metric\n"); 
            }
          
          geo_info.visibility_counters[j].visible_ips = 0;
          snprintf(buffer, BUFFER_LEN, METRIC_PREFIX_TH_FORMAT,
                   CHAIN_STATE->metric_prefix, countries[i]->continent, countries[i]->iso2,
                   bgpstream_ipv2number(BGPSTREAM_ADDR_VERSION_IPV4),
                   threshold_string(j), "visible_ips_cnt");
          if((geo_info.visible_ips_idx[j] = timeseries_kp_add_key(STATE->kp_v4, buffer)) == -1)
            {
              fprintf(stderr, "ERROR: Could not create key metric\n"); 
            }
          
          geo_info.visibility_counters[j].ff_peer_asns_sum = 0;
          snprintf(buffer, BUFFER_LEN, METRIC_PREFIX_TH_FORMAT,
                   CHAIN_STATE->metric_prefix, countries[i]->continent, countries[i]->iso2,
                   bgpstream_ipv2number(BGPSTREAM_ADDR_VERSION_IPV4),
                   threshold_string(j), "ff_peer_asns_sum");             
          if((geo_info.ff_peer_asns_sum_idx[j] = timeseries_kp_add_key(STATE->kp_v4, buffer)) == -1)
            {
              fprintf(stderr, "ERROR: Could not create key metric\n"); 
            }
        }
  
      kh_value(STATE->countrycode_pfxs, k) = geo_info;
      
    }

  return 0;
}

static int create_gen_metrics(bvc_t *consumer)
{
  char buffer[BUFFER_LEN];

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "cache_miss_cnt");
  if((STATE->gen_metrics.cache_misses_cnt_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "cache_hit_cnt");
  if((STATE->gen_metrics.cache_hits_cnt_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "arrival_delay");
  if((STATE->gen_metrics.arrival_delay_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "processed_delay");
  if((STATE->gen_metrics.processed_delay_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "processing_time");
  if((STATE->gen_metrics.processing_time_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "max_numcountries_perpfx");
  if((STATE->gen_metrics.max_numcountries_perpfx_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "avg_numcountries_perpfx");
  if((STATE->gen_metrics.avg_numcountries_perpfx_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "visible_pfxs_cnt");
    if((STATE->gen_metrics.num_visible_pfx_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

  snprintf(buffer, BUFFER_LEN, META_METRIC_PREFIX_FORMAT,
           CHAIN_STATE->metric_prefix, "max_records_perpfx");
    if((STATE->gen_metrics.max_records_perpfx_idx =
      timeseries_kp_add_key(STATE->kp_gen, buffer)) == -1)
    {
      return -1;
    }

    return 0;
}

static void dump_gen_metrics(bvc_t *consumer)
{

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.cache_misses_cnt_idx,
                    STATE->cache_misses_cnt);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.cache_hits_cnt_idx,
                    STATE->cache_hits_cnt);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.arrival_delay_idx,
                    STATE->arrival_delay);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.processed_delay_idx,
                    STATE->processed_delay);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.processing_time_idx,
                    STATE->processing_time);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.max_numcountries_perpfx_idx,
                    STATE->max_numcountries_perpfx);

  if(STATE->num_visible_pfx > 0)
    {
      STATE->avg_numcountries_perpfx =
        STATE->avg_numcountries_perpfx / (double) STATE->num_visible_pfx;
    }

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.avg_numcountries_perpfx_idx,
                    STATE->avg_numcountries_perpfx);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.num_visible_pfx_idx,
                    STATE->num_visible_pfx);

  timeseries_kp_set(STATE->kp_gen, STATE->gen_metrics.max_records_perpfx_idx,
                    STATE->max_records_perpfx);

  STATE->cache_misses_cnt = 0;
  STATE->cache_hits_cnt = 0;
  STATE->arrival_delay = 0;
  STATE->processed_delay = 0;
  STATE->processing_time = 0;
  STATE->max_numcountries_perpfx = 0;
  STATE->avg_numcountries_perpfx = 0;
  STATE->num_visible_pfx = 0;
  // we do not reset STATE->max_records_perpfx

}

static void dump_v4table(bvc_t *consumer)
{
  khiter_t k;
  pergeo_info_t *info;
  int i;
  for (k = kh_begin(STATE->countrycode_pfxs);
       k != kh_end(STATE->countrycode_pfxs); ++k)
    {
      if (kh_exist(STATE->countrycode_pfxs, k))
	{
          info = &kh_val(STATE->countrycode_pfxs, k);

	  bgpstream_ipv4_pfx_set_clear(info->v4pfxs);

          timeseries_kp_set(STATE->kp_v4, info->asns_idx, bgpstream_id_set_size(info->asns));
	  bgpstream_id_set_clear(info->asns);

          for(i=0; i<VIS_THRESHOLDS_CNT; i++)
            {
              timeseries_kp_set(STATE->kp_v4, info->visible_pfxs_idx[i], info->visibility_counters[i].visible_pfxs);
              info->visibility_counters[i].visible_pfxs = 0;

              timeseries_kp_set(STATE->kp_v4, info->visible_ips_idx[i], info->visibility_counters[i].visible_ips);
              info->visibility_counters[i].visible_ips = 0;

              timeseries_kp_set(STATE->kp_v4, info->ff_peer_asns_sum_idx[i], info->visibility_counters[i].ff_peer_asns_sum);
              info->visibility_counters[i].ff_peer_asns_sum = 0;

            }
	}
    }
}


KHASH_INIT(country_k_set /* name */,
	   uint32_t  /* khkey_t */,
	   char /* khval_t */,
	   0  /* kh_is_set */,
	   kh_int_hash_func /*__hash_func */,
	   kh_int_hash_equal /* __hash_equal */);


static void geotag_v4table(bvc_t *consumer, bgpview_iter_t *it)
{

  /* fprintf(stderr, "geotag_v4table START: \n"); */

  bgpstream_pfx_t *pfx;
  bgpstream_peer_id_t peerid;
  bgpstream_peer_sig_t *sg;
  khiter_t k;
  khiter_t setk;
  khiter_t cck;
  khiter_t idk;
  int khret;
  const int i = bgpstream_ipv2idx(BGPSTREAM_ADDR_VERSION_IPV4);

  /* full feed asns observing a prefix */
  bgpstream_id_set_t *ff_asns = bgpstream_id_set_create();
  int asns_count = 0;

  /*  origin ases 
   * (as seen by full feed peers) */
  bgpstream_id_set_t *ff_origin_asns = bgpstream_id_set_create();


  khash_t(country_k_set) *cck_set = NULL;
  pergeo_info_t *geo_info;
  ipmeta_record_t *rec;
  uint32_t num_ips;
  int num_records;

  bgpstream_as_path_seg_t *origin_seg;
  uint32_t origin_asn;

  for(bgpview_iter_first_pfx(it, BGPSTREAM_ADDR_VERSION_IPV4, BGPVIEW_FIELD_ACTIVE);
      bgpview_iter_has_more_pfx(it);
      bgpview_iter_next_pfx(it))
    {

      /* get the current v4 prefix */
      pfx = bgpview_iter_pfx_get_pfx(it);

      // WARNING we do not geolocate ipv6 prefixes
      assert(pfx->address.version == BGPSTREAM_ADDR_VERSION_IPV4);

        /* only consider ipv4 prefixes whose mask is shorter than a /6 */
      if(pfx->address.version == BGPSTREAM_ADDR_VERSION_IPV4 &&
         pfx->mask_len < BVC_GET_CHAIN_STATE(consumer)->pfx_vis_mask_len_threshold)
        {
          continue;
        }
      
      /* iterate over the peers for the current pfx and get the number of unique
       * full feed AS numbers observing this prefix as well as the unique set of
       * origin ASes */
      for(bgpview_iter_pfx_first_peer(it, BGPVIEW_FIELD_ACTIVE);
          bgpview_iter_pfx_has_more_peer(it);
          bgpview_iter_pfx_next_peer(it))
        {
          /* only consider peers that are full-feed */
          peerid = bgpview_iter_peer_get_peer_id(it);
          sg = bgpview_iter_peer_get_sig(it);          
          
            if(bgpstream_id_set_exists(BVC_GET_CHAIN_STATE(consumer)->full_feed_peer_ids[i],
                                     peerid) == 0)
            {
              continue;
            }
          
          bgpstream_id_set_insert(ff_asns, sg->peer_asnumber);

          /** @todo correctly handle origin sets */
          if((origin_seg =
              bgpview_iter_pfx_peer_get_origin_seg(it)) == NULL ||
             origin_seg->type != BGPSTREAM_AS_PATH_SEG_ASN)
            {
              origin_asn = 0;
            }
          else
            {
              origin_asn = ((bgpstream_as_path_seg_asn_t*)origin_seg)->asn;
            }

          bgpstream_id_set_insert(ff_origin_asns, origin_asn);
        }

      asns_count = bgpstream_id_set_size(ff_asns);

      STATE->num_visible_pfx++;

      // First we check if this prefix has been already
      // geotagged in previous iterations

      cck_set = (khash_t(country_k_set) *) bgpview_iter_pfx_get_user(it);

      // if the set is null, then we need to initialize the set
      // and proceed with the geolocation
      if(cck_set == NULL)
        {
          STATE->cache_misses_cnt++;

          // a new set has to be created, with the geolocation
          // information
          if( (cck_set = kh_init(country_k_set)) == NULL)
            {
              fprintf(stderr, "Error: cannot create country_k_set\n");
              return;
            }

          // then we link this set to the appropriate user ptr
          bgpview_iter_pfx_set_user(it, (void *) cck_set);

          // geolocation
          ipmeta_lookup(STATE->provider, (uint32_t) ((bgpstream_ipv4_pfx_t *)pfx)->address.ipv4.s_addr,
                        pfx->mask_len, STATE->records);
          ipmeta_record_set_rewind(STATE->records);
          num_records = 0;
          while ( (rec = ipmeta_record_set_next(STATE->records, &num_ips)) )
            {
              num_records++;

              // check that we already had this country in our dataset
              if((k = kh_get(cc_pfxs, STATE->countrycode_pfxs,
                             rec->country_code)) ==
                 kh_end(STATE->countrycode_pfxs))
                {
                  fprintf(stderr, "Warning: country (%s) not found\n",
                          rec->country_code);
                }
              else
                {
                  // insert k in the country-code-k set
                  if((setk = kh_get(country_k_set, cck_set, k)) == kh_end(cck_set))
                    {
                      setk = kh_put(country_k_set, cck_set, k, &khret);                              
                    }                  
                }
            }

          if(num_records > STATE->max_records_perpfx)
            {
              STATE->max_records_perpfx = num_records;
            }
        }
      else
        {
          STATE->cache_hits_cnt++;
        }


      // Whether the cck_set already existed, or it has just been created,
      // we update the geo counters

      // geolocation already performed, then proceed and increment
      // the counters for each country
      // cc_k_set contains the k position of a country in the
      // countrycode_pfxs hash map

      uint8_t net_size = 32 - pfx->mask_len;
      
      for(idk = kh_begin(cck_set);
          idk != kh_end(cck_set); ++idk)
        {
          if (kh_exist(cck_set, idk))
            {
              cck = kh_key(cck_set, idk);
              geo_info = &kh_value(STATE->countrycode_pfxs, cck);
              bgpstream_ipv4_pfx_set_insert(geo_info->v4pfxs, (bgpstream_ipv4_pfx_t *)pfx);
              
              update_visibility_counters(geo_info->visibility_counters, net_size, asns_count,
                                         BVC_GET_CHAIN_STATE(consumer)->full_feed_peer_asns_cnt[i]);

              bgpstream_id_set_merge(geo_info->asns, ff_origin_asns);
              STATE->avg_numcountries_perpfx++;
            }
        }

      if(kh_size(cck_set) > STATE->max_numcountries_perpfx)
        {
          STATE->max_numcountries_perpfx = kh_size(cck_set);
        }

      bgpstream_id_set_clear(ff_asns);
      bgpstream_id_set_clear(ff_origin_asns);

    } /* end per-pfx loop */

  bgpstream_id_set_destroy(ff_asns);          
  bgpstream_id_set_destroy(ff_origin_asns);          

}
  
  


/* ==================== CONSUMER INTERFACE FUNCTIONS ==================== */

bvc_t *bvc_pergeovisibility_alloc()
{
  return &bvc_pergeovisibility;
}

int bvc_pergeovisibility_init(bvc_t *consumer, int argc, char **argv)
{
  bvc_pergeovisibility_state_t *state = NULL;

  if((state = malloc_zero(sizeof(bvc_pergeovisibility_state_t))) == NULL)
    {
      return -1;
    }
  BVC_SET_STATE(consumer, state);

  /* set defaults here */

  // change it! (str -> set(pfxs))
  if((state->countrycode_pfxs = kh_init(cc_pfxs)) == NULL)
    {
      fprintf(stderr, "Error: Unable to create cc visibility map\n");
      goto err;
    }

  if((state->kp_gen =
      timeseries_kp_init(BVC_GET_TIMESERIES(consumer), 1)) == NULL)
    {
      fprintf(stderr, "Error: Could not create timeseries key package (gen)\n");
      goto err;
    }

  if((state->kp_v4 =
      timeseries_kp_init(BVC_GET_TIMESERIES(consumer), 1)) == NULL)
    {
      fprintf(stderr, "Error: Could not create timeseries key package (v4)\n");
      goto err;
    }

  /* initialize ipmeta structure */
  if((state->ipmeta = ipmeta_init()) == NULL)
    {
      fprintf(stderr, "Error: Could not initialize ipmeta \n");
      goto err;
    }

  /* parse the command line args */
  if(parse_args(consumer, argc, argv) != 0)
    {
      goto err;
    }

  /* initialize ipmeta and provider */
  if(init_ipmeta(consumer) != 0)
    {
      goto err;
    }

  /* create a timeseries metric for each country */
  if(create_per_cc_metrics(consumer) != 0)
    {
      goto err;
    }

  /* create the top-level general metrics, and meta metrics */
  if(create_gen_metrics(consumer) != 0)
    {
      goto err;
    }

  return 0;

 err:
  bvc_pergeovisibility_destroy(consumer);
  return -1;
}


static void
bvc_destroy_pfx_user_ptr(void *user)
{
  khash_t(country_k_set) *cck_set = (khash_t(country_k_set) *) user;
  kh_destroy(country_k_set, cck_set);  
}


void bvc_pergeovisibility_destroy(bvc_t *consumer)
{

  bvc_pergeovisibility_state_t *state = STATE;

  if(state == NULL)
    {
      return;
    }

  /* destroy things here */
  khiter_t idk;
  if(state->countrycode_pfxs != NULL)
    {
      for(idk = kh_begin(state->countrycode_pfxs);
          idk != kh_end(state->countrycode_pfxs); ++idk)
        {
          if (kh_exist(state->countrycode_pfxs, idk))
            {
              free(kh_key(state->countrycode_pfxs, idk));
              pergeo_info_destroy(kh_value(state->countrycode_pfxs, idk));
            }
        }
      kh_destroy(cc_pfxs, state->countrycode_pfxs);
      state->countrycode_pfxs = NULL;
    }

  timeseries_kp_free(&state->kp_gen);
  timeseries_kp_free(&state->kp_v4);

  if(state->ipmeta != NULL)
    {
      ipmeta_free(state->ipmeta);
      state->ipmeta = NULL;
    }

  if(state->records != NULL)
    {
      ipmeta_record_set_free(&state->records);
      state->records = NULL;
    }

  free(state);

  BVC_SET_STATE(consumer, NULL);
}



int bvc_pergeovisibility_process_view(bvc_t *consumer, uint8_t interests,
                                      bgpview_t *view)
{
  bgpview_iter_t *it;

  if(BVC_GET_CHAIN_STATE(consumer)->visibility_computed == 0)
    {
      fprintf(stderr,
              "ERROR: The Per-AS Visibility requires the Visibility consumer "
              "to be run first\n");
      return -1;
    }

  // compute arrival delay
  STATE->arrival_delay = zclock_time()/1000- bgpview_get_time(view);

  /* create a new iterator */
  if((it = bgpview_iter_create(view)) == NULL)
    {
      return -1;
    }

  bgpview_set_pfx_user_destructor(view, bvc_destroy_pfx_user_ptr);
  
  int i = bgpstream_ipv2idx(BGPSTREAM_ADDR_VERSION_IPV4);

  if(BVC_GET_CHAIN_STATE(consumer)->usable_table_flag[i] != 0)
    {
      /* analyze v4 table */
      geotag_v4table(consumer, it);

      dump_v4table(consumer);

      /* now flush the kp */
      if(timeseries_kp_flush(STATE->kp_v4, bgpview_get_time(view)) != 0)
        {
          return -1;
        }
    }

  bgpview_iter_destroy(it);

  // compute processed delay (must come prior to dump_gen_metrics)
  STATE->processed_delay = zclock_time()/1000- bgpview_get_time(view);
  STATE->processing_time = STATE->processed_delay - STATE->arrival_delay;

  /* dump metrics and tables */
  dump_gen_metrics(consumer);

  /* now flush the kp */
  if(timeseries_kp_flush(STATE->kp_gen, bgpview_get_time(view)) != 0)
    {
      return -1;
    }

  return 0;
}
