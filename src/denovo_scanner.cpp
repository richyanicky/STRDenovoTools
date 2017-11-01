/*
Copyright (C) 2017 Melissa Gymrek <mgymrek@ucsd.edu>

This file is part of STRDenovoTools.

STRDenovoTools is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

STRDenovoTools is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with STRDenovoTools.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>

#include <cfloat>
#include <sstream>
#include <vector>

#include "src/denovo_allele_priors.h"
#include "src/denovo_scanner.h"
#include "src/mathops.h"
#include "src/mutation_model.h"
#include "src/vcf_input.h"

#include "lib/htslib/htslib/kfunc.h"

std::string TrioDenovoScanner::START_KEY   = "START";
std::string TrioDenovoScanner::END_KEY     = "END";
std::string TrioDenovoScanner::PERIOD_KEY  = "PERIOD";

TrioDenovoScanner::~TrioDenovoScanner() {
  locus_summary_.close();
  all_mutations_file_.close();
}

void TrioDenovoScanner::scan(VCF::VCFReader& strvcf,
			     MutationPriors& priors) {
  VCF::Variant str_variant;
  std::vector<NuclearFamily> families = pedigree_set_.get_families();
  std::vector<DenovoResult> denovo_results;
  while (strvcf.get_next_variant(&str_variant, options_.round_alleles)) {
    denovo_results.clear();
    // Initial checks
    int num_alleles = str_variant.num_alleles();
    if (options_.combine_alleles) {
      num_alleles = str_variant.num_alleles_by_length(options_.round_alleles);
    }
    if (num_alleles <= 1) {
      str_variant.destroy_record();
      continue;
    }
    if (str_variant.num_samples() == str_variant.num_missing()) {
      str_variant.destroy_record();
      continue;
    }
    if (num_alleles > options_.max_num_alleles) {
      str_variant.destroy_record();
      continue;
    }
    // Get locus info
    int32_t start; str_variant.get_INFO_value_single_int(START_KEY, start);
    int32_t end; str_variant.get_INFO_value_single_int(END_KEY, end);
    int32_t period; str_variant.get_INFO_value_single_int(PERIOD_KEY, period);
    if (options_.period != 0 && period != options_.period) {
      str_variant.destroy_record();
      continue;
    }
    std::stringstream ss;
    ss << "Processing STR region " << str_variant.get_chromosome() << ":" << start << "-" << end
       << " with " << num_alleles << " alleles.";
    if (options_.combine_alleles) {
      ss << " Total allele seqs: " << str_variant.num_alleles();
    }
    PrintMessageDieOnError(ss.str(), M_PROGRESS);

    // Set up
    GL* unphased_gls;
    UnphasedLengthGL unphased_length_gls(str_variant, options_);
    UnphasedGL unphased_seq_gls(str_variant, options_);
    MutationModel mut_model(str_variant, priors, options_);
    DiploidGenotypePrior* dip_gt_priors;
    if (options_.combine_alleles) {
      //unphased_gls = new UnphasedLengthGL(str_variant, options_);
      unphased_gls = &unphased_length_gls;
      if (options_.use_pop_priors) {
	dip_gt_priors = new PopulationGenotypeLengthPrior(str_variant, families, options_.round_alleles);
      } else {
	dip_gt_priors = new UniformGenotypeLengthPrior(str_variant, families, options_.round_alleles);
      }
    } else {
      //unphased_gls = new UnphasedGL(str_variant, options_);
      unphased_gls = &unphased_seq_gls;
      if (options_.use_pop_priors) {
	dip_gt_priors = new PopulationGenotypePrior(str_variant, families);
      } else {
	dip_gt_priors = new UniformGenotypePrior(str_variant, families);
      }
    }
    double prior_rate = priors.GetPrior(str_variant.get_chromosome(), start);
    const double LOG_ONE_FOURTH = -log10(4);
    const double LOG_TWO        = log10(2);

    // Scan each family
    for (auto family_iter = families.begin(); family_iter != families.end(); family_iter++) {
      if (!options_.family.empty() and family_iter->get_family_id() != options_.family) {
	continue;
      }
      bool scan_for_denovo = unphased_gls->has_sample(family_iter->get_mother()) && unphased_gls->has_sample(family_iter->get_father());
      if (options_.require_all_children) {
	for (auto child_iter = family_iter->get_children().begin();
	     child_iter != family_iter->get_children().end(); child_iter++) {
	  scan_for_denovo = scan_for_denovo && unphased_gls->has_sample(*child_iter);
	}
      }
      // Scan each child in the family
      for (auto child_iter = family_iter->get_children().begin(); child_iter != family_iter->get_children().end(); child_iter++) {
	if (!scan_for_denovo || !unphased_gls->has_sample(*child_iter)) {
	  continue;
	}
	// To accelerate computations, we will ignore configurations that make a neglible contribution (< 0.01%) to the total LL
	// For mutational scenarios, we aggregate 1/4*A^2*(A+1)^2*4*2*A values. Therefore, to ignore a configuration with LL=X:
	// X*A^3*(A+1)^2*2 < TOTAL/10000;
	// logX < log(TOTAL) - log(10000*A^3*(A+1)^2*2) = log(TOTAL) - [log(10000) + 3log(A) + 2log(A+1) + log(2)];
	double MIN_CONTRIBUTION   = 4 + 3*log10(num_alleles) + 2*log(num_alleles+1) + LOG_TWO;
	double ll_no_mutation_max = -DBL_MAX/2, ll_no_mutation_total = 0.0;
	double ll_one_denovo_max  = -DBL_MAX/2, ll_one_denovo_total  = 0.0;
	int mother_gl_index       = unphased_gls->get_sample_index(family_iter->get_mother());
	int father_gl_index       = unphased_gls->get_sample_index(family_iter->get_father());
	int child_gl_index        = unphased_gls->get_sample_index(*child_iter);

	// Iterate over all maternal genotypes
	for (int mat_i = 0; mat_i < num_alleles; mat_i++){
	  for (int mat_j = 0; mat_j <= mat_i; mat_j++){
	    double mat_ll = dip_gt_priors->log_unphased_genotype_prior(mat_j, mat_i, family_iter->get_mother()) + unphased_gls->get_gl(mother_gl_index, mat_j, mat_i);

	    // Iterate over all paternal genotypes
	    for (int pat_i = 0; pat_i < num_alleles; pat_i++){
	      for (int pat_j = 0; pat_j <= pat_i; pat_j++){
		double pat_ll    = dip_gt_priors->log_unphased_genotype_prior(pat_j, pat_i, family_iter->get_father()) + unphased_gls->get_gl(father_gl_index, pat_j, pat_i);
		double config_ll = mat_ll + pat_ll + LOG_ONE_FOURTH;

		// Iterate over all 4 possible inheritance patterns for the child
		for (int mat_index = 0; mat_index < 2; ++mat_index){
		  int mat_allele = (mat_index == 0 ? mat_i : mat_j);
		  for (int pat_index = 0; pat_index < 2; ++pat_index){
		    int pat_allele = (pat_index == 0 ? pat_i : pat_j);

		    double no_mutation_config_ll = config_ll + unphased_gls->get_gl(child_gl_index, std::min(mat_allele, pat_allele), std::max(mat_allele, pat_allele));
		    update_streaming_log_sum_exp(no_mutation_config_ll, ll_no_mutation_max, ll_no_mutation_total);

		    // All putative mutations to the maternal allele
		    double max_ll_mat_mut = config_ll + unphased_gls->get_max_gl_allele_fixed(child_gl_index, pat_allele) + 
		      mut_model.max_log_prior_mutation(str_variant.GetSizeFromLengthAllele(mat_allele));
		    if (max_ll_mat_mut > ll_one_denovo_max-MIN_CONTRIBUTION){
		      for (int mut_allele = 0; mut_allele < num_alleles; mut_allele++){
			if (mut_allele == mat_allele)
			  continue;
			double prob = config_ll + unphased_gls->get_gl(child_gl_index, std::min(mut_allele, pat_allele), 
								      std::max(mut_allele, pat_allele))
			  + mut_model.log_prior_mutation(str_variant.GetSizeFromLengthAllele(mat_allele),
							 str_variant.GetSizeFromLengthAllele(mut_allele));
			update_streaming_log_sum_exp(prob, ll_one_denovo_max, ll_one_denovo_total);
		      }
		    }

		    // All putative mutations to the paternal allele
		    double max_ll_pat_mut = config_ll + unphased_gls->get_max_gl_allele_fixed(child_gl_index, mat_allele) + 
		      mut_model.max_log_prior_mutation(str_variant.GetSizeFromLengthAllele(pat_allele));
		    if (max_ll_pat_mut > ll_one_denovo_max - MIN_CONTRIBUTION){
		      for (int mut_allele = 0; mut_allele < num_alleles; mut_allele++){
			if (mut_allele == pat_allele)
			  continue;
			double prob = config_ll + unphased_gls->get_gl(child_gl_index, std::min(mat_allele, mut_allele), std::max(mat_allele, mut_allele))
			  + mut_model.log_prior_mutation(str_variant.GetSizeFromLengthAllele(pat_allele),
							 str_variant.GetSizeFromLengthAllele(mut_allele));
			update_streaming_log_sum_exp(prob, ll_one_denovo_max, ll_one_denovo_total);
		      }
		    }
		  }
		}
	      }
	    }
	  }
	}

	// Compute total LL for each scenario
	double total_ll_no_mutation = finish_streaming_log_sum_exp(ll_no_mutation_max, ll_no_mutation_total);
	double total_ll_one_denovo  = finish_streaming_log_sum_exp(ll_one_denovo_max,  ll_one_denovo_total);
	
	// Add to results
	DenovoResult dnr(family_iter->get_family_id(), family_iter->get_mother(), family_iter->get_father(),
			 (*child_iter), family_iter->get_child_phenotype(*child_iter),
			 total_ll_no_mutation, total_ll_one_denovo, prior_rate);
	denovo_results.push_back(dnr);
      }
    }
    summarize_results(denovo_results, str_variant);
    delete dip_gt_priors;
    denovo_results.clear();
    
    // Destroy vcf_record_ from variant
    str_variant.destroy_record();
  }
}

/*
  Try to determine the de novo allele and how big the step size was
  If we can't figure it out, return "NA" for new_allele and mut_size
 */
void TrioDenovoScanner::GetMutationInfo(const VCF::Variant& variant, const std::string& mother_id,
					const std::string& father_id, const std::string& child_id,
					std::string* new_allele, std::string* mut_size,
					bool* new_allele_in_parents, int* poocase) {
  int gt_mother_a, gt_mother_b, gt_father_a, gt_father_b, gt_child_a, gt_child_b;
  int ref_allele_size = (int)variant.get_allele(0).size();
  *new_allele = "NA";
  *mut_size = "NA";
  *poocase = -1;
  variant.get_genotype(mother_id, gt_mother_a, gt_mother_b);
  variant.get_genotype(father_id, gt_father_a, gt_father_b);
  variant.get_genotype(child_id, gt_child_a, gt_child_b);
  // Case 1: Mendelian - skip
  if ((gt_child_a == gt_mother_a || gt_child_a == gt_mother_b) &&
      (gt_child_b == gt_father_a || gt_child_b == gt_father_b)) {
    *new_allele_in_parents = true;
    *poocase = 1;
    return;
  }
  if ((gt_child_a == gt_father_a || gt_child_a == gt_father_b) && 
      (gt_child_b == gt_mother_a || gt_child_b == gt_mother_b)) {
    *new_allele_in_parents = true;
    *poocase = 1;
    return;
  }
  // Case 2: New allele from father
  // Allele a in mother only, allele b not in father
  if ((gt_child_a == gt_mother_a || gt_child_a == gt_mother_b) &&
      (gt_child_a != gt_father_a && gt_child_a != gt_father_b) &&
      (gt_child_b != gt_father_a && gt_child_b != gt_father_b)) {
    *new_allele = std::to_string((int)variant.get_allele(gt_child_b).size()-ref_allele_size);
    int diff1 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_father_a).size();
    int diff2 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_father_b).size();
    if (abs(diff1) < abs(diff2)) {
      *mut_size = std::to_string(diff1);
    } else {
      *mut_size = std::to_string(diff2);
    }
    *poocase = 2;
    return;
  }
  // Allele b in mother only, allele a not in father
  if ((gt_child_b == gt_mother_a || gt_child_b == gt_mother_b) &&
      (gt_child_b != gt_father_a && gt_child_b != gt_father_b) &&
      gt_child_a != gt_father_a && gt_child_a != gt_father_b) {
    *new_allele = std::to_string((int)variant.get_allele(gt_child_a).size()-ref_allele_size);
    int diff1 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_father_a).size();
    int diff2 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_father_b).size();
    if (abs(diff1) < abs(diff2)) {
      *mut_size = std::to_string(diff1);
    } else {
      *mut_size = std::to_string(diff2);
    }
    *poocase = 21;
    return;
  }
  // Case 3: New allele from mother
  // Allele a in father only, allele b not in mother
  if ((gt_child_a == gt_father_a || gt_child_a == gt_father_b) &&
      (gt_child_a != gt_mother_a && gt_child_a != gt_mother_b) &&
      (gt_child_b != gt_mother_a && gt_child_b != gt_mother_b)) {
    *new_allele = std::to_string((int)variant.get_allele(gt_child_b).size()-ref_allele_size);
    int diff1 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_mother_a).size();
    int diff2 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_mother_b).size();
    if (abs(diff1) < abs(diff2)) {
      *mut_size = std::to_string(diff1);
    } else {
      *mut_size = std::to_string(diff2);
    }
    *poocase = 3;
    return;
  }
  // Allele b in father only, allele a not in mother
  if ((gt_child_b == gt_father_a || gt_child_b == gt_father_b) &&
      (gt_child_b != gt_mother_a && gt_child_b != gt_mother_b) &&
      (gt_child_a != gt_mother_a && gt_child_a != gt_mother_b)) {
    *new_allele = std::to_string((int)variant.get_allele(gt_child_a).size()-ref_allele_size);
    int diff1 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_mother_a).size();
    int diff2 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_mother_b).size();
    if (abs(diff1) < abs(diff2)) {
      *mut_size = std::to_string(diff1);
    } else {
      *mut_size = std::to_string(diff2);
    }
    *poocase = 31;
    return;
  }
  // Case 4: Not clear, pick closest allele that is not equal
  if (gt_child_a != gt_mother_a && gt_child_a != gt_mother_b &&
      gt_child_a != gt_father_a && gt_child_a != gt_father_b) {
    *new_allele = std::to_string((int)variant.get_allele(gt_child_a).size()-ref_allele_size);
    int diff1 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_mother_a).size();
    int diff2 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_mother_b).size();
    int diff3 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_father_a).size();
    int diff4 = (int)variant.get_allele(gt_child_a).size()-(int)variant.get_allele(gt_father_b).size();
    int min_size = std::min(std::min(abs(diff1), abs(diff2)), std::min(abs(diff3), abs(diff4)));
    if (min_size == abs(diff1)) {
      *mut_size = std::to_string(diff1);
    } else if (min_size == abs(diff2)) {
      *mut_size = std::to_string(diff2);
    } else if (min_size == abs(diff3)) {
      *mut_size = std::to_string(diff3);
    } else {
      *mut_size = std::to_string(diff4);
    }
    *poocase = 4;
    return;
  }
  if (gt_child_b != gt_mother_a && gt_child_b != gt_mother_b &&
      gt_child_b != gt_father_a && gt_child_b != gt_father_b) {
    *new_allele = std::to_string((int)variant.get_allele(gt_child_b).size()-ref_allele_size);
    int diff1 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_mother_a).size();
    int diff2 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_mother_b).size();
    int diff3 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_father_a).size();
    int diff4 = (int)variant.get_allele(gt_child_b).size()-(int)variant.get_allele(gt_father_b).size();
    int min_size = std::min(std::min(abs(diff1), abs(diff2)), std::min(abs(diff3), abs(diff4)));
    if (min_size == abs(diff1)) {
      *mut_size = std::to_string(diff1);
    } else if (min_size == abs(diff2)) {
      *mut_size = std::to_string(diff2);
    } else if (min_size == abs(diff3)) {
      *mut_size = std::to_string(diff3);
    } else {
      *mut_size = std::to_string(diff4);
    }
    *poocase = 41;
    return;
  }
  *poocase = 5;
  return;
}

void TrioDenovoScanner::summarize_results(std::vector<DenovoResult>& dnr,
					  VCF::Variant& str_variant) {
  if (dnr.empty()) {
    stringstream ss;
    ss << " Skipping - no called children";
    PrintMessageDieOnError(ss.str(), M_WARNING);
    return;
  }
  // Get locus sinfo
  int32_t start; str_variant.get_INFO_value_single_int(START_KEY, start);
  int32_t end; str_variant.get_INFO_value_single_int(END_KEY, end);
  int32_t period; str_variant.get_INFO_value_single_int(PERIOD_KEY, period);
  // Get mutation info
  int total_children = 0;
  int total_unaffected = 0;
  int total_affected = 0;
  int num_mutations = 0;
  int num_mutations_unaffected = 0;
  int num_mutations_affected = 0;
  std::vector<std::string>children_with_mutations;
  for (auto dnr_iter = dnr.begin(); dnr_iter != dnr.end(); dnr_iter++) {
    total_children++;
    std::string new_allele, mut_size;
    bool new_allele_in_parents = false;
    int poocase;
    GetMutationInfo(str_variant, dnr_iter->get_mother_id(), dnr_iter->get_father_id(),
		    dnr_iter->get_child_id(), &new_allele, &mut_size,
		    &new_allele_in_parents, &poocase);
    if (dnr_iter->get_posterior() > options_.posterior_threshold || options_.outputall) {
      all_mutations_file_ << str_variant.get_chromosome() << "\t" << start << "\t"
			  << period  << "\t" << pow(10, dnr_iter->get_prior()) << "\t"
			  << dnr_iter->get_family_id() << "\t" << dnr_iter->get_child_id() << "\t"
			  << dnr_iter->get_phenotype() << "\t" << dnr_iter->get_posterior() << "\t"
			  << new_allele << "\t" << mut_size << "\t"
			  << new_allele_in_parents << "\t" << poocase
			  << "\n";
      all_mutations_file_.flush();
    }
    if (dnr_iter->get_posterior() > options_.posterior_threshold) {
      num_mutations++;
      children_with_mutations.push_back(dnr_iter->get_family_id() + ":" + dnr_iter->get_child_id());
      if (dnr_iter->get_phenotype() == PT_CONTROL) {
	num_mutations_unaffected++;
      }
      if (dnr_iter->get_phenotype() == PT_CASE) {
	num_mutations_affected++;
      }
    }
    if (dnr_iter->get_phenotype() == PT_CONTROL) {
      total_unaffected++;
    }
    if (dnr_iter->get_phenotype() == PT_CASE) {
      total_affected++;
    }
  }
  double total_mutation_rate = (double)(num_mutations)/(double)total_children;
  double affected_mutation_rate = (double)(num_mutations_affected)/(double)total_affected;
  double unaffected_mutation_rate = (double)(num_mutations_unaffected)/(double)total_unaffected;
  // Perform fisher exact test
  double fisher_left_p, fisher_right_p, fisher_twosided_p;
  int n11 = num_mutations_affected;
  int n12 = total_affected - num_mutations_affected;
  int n21 = num_mutations_unaffected;
  int n22 = total_unaffected - num_mutations_unaffected;
  kt_fisher_exact(n11, n12, n21, n22, &fisher_left_p, &fisher_right_p, &fisher_twosided_p);
  double p_value = fisher_right_p;
  // Output status
  if (num_mutations > 0) {
    std::stringstream ss;
    ss << "   Analyzed " << total_children << " children\n"
       << "      Total mutation rate: " << total_mutation_rate
       << " (" << num_mutations << "/" << total_children << ")\n"
       << "      Affected mutation rate: " << affected_mutation_rate
       << " (" << num_mutations_affected << "/" << total_affected << ")\n"
       << "      Unaffected mutation rate: " << unaffected_mutation_rate
       << " (" << num_mutations_unaffected << "/" << total_unaffected << ")\n"
       << " P-value: " << p_value;
    PrintMessageDieOnError(ss.str(), M_PROGRESS);
  }
  // Write output file
  std::string children_with_mutations_string = ".";
  if (children_with_mutations.size() > 0) {
    join(&children_with_mutations_string, children_with_mutations, ",");
  }
  locus_summary_ << str_variant.get_chromosome() << "\t"
		 << start << "\t" << end << "\t" << period << "\t"
		 << str_variant.num_alleles_by_length(options_.round_alleles) << "\t" << str_variant.num_alleles()  << "\t"
		 << str_variant.heterozygosity_by_length(options_.round_alleles) << "\t" << str_variant.heterozygosity() << "\t"
		 << total_children << "\t" << num_mutations << "\t" << total_mutation_rate << "\t"
		 << total_affected << "\t" << num_mutations_affected << "\t" << affected_mutation_rate << "\t"
		 << total_unaffected << "\t" << num_mutations_unaffected << "\t" << unaffected_mutation_rate << "\t"
		 << p_value << "\t" << children_with_mutations_string << "\n";
  locus_summary_.flush();
}

DenovoResult::DenovoResult(const std::string& family_id,
			   const std::string& mother_id,
			   const std::string& father_id,
			   const std::string& child_id,
			   const int& phenotype,
			   const double& total_ll_no_mutation,
			   const double& total_ll_one_denovo,
			   const double& log10prior) {
  family_id_ = family_id;
  mother_id_ = mother_id;
  father_id_ = father_id;
  child_id_ = child_id;
  phenotype_ = phenotype;
  total_ll_no_mutation_ = total_ll_no_mutation;
  total_ll_one_denovo_ = total_ll_one_denovo;
  log10_prior_mutation_ = log10prior;
  CalculatePosterior();
}

void DenovoResult::CalculatePosterior() {
  double log10_prior_nomut = log10(1-pow(10, log10_prior_mutation_));
  // Note, all likelihoods in log10, but fast_log_sum_exp assumes log_e
  double denom = fast_log_sum_exp((total_ll_one_denovo_+log10_prior_mutation_)*log(10),
				  (total_ll_no_mutation_+log10_prior_nomut)*log(10))*log10(exp(1)); // Converts denom to log10
  double posterior = pow(10, total_ll_one_denovo_+log10_prior_mutation_ - denom);
  posterior_ = posterior;
}

DenovoResult::~DenovoResult() {}
