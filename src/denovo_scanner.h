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

#ifndef TRIO_DENOVO_SCANNER_H_
#define TRIO_DENOVO_SCANNER_H_

#include <assert.h>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

#include "src/options.h"
#include "src/pedigree.h"
#include "src/vcf_reader.h"

using namespace std;

class DenovoResult {
 public:
  DenovoResult(const std::string& family_id,
	       const std::string& child_id,
	       const int& phenotype,
	       const double& total_ll_no_mutation,
	       const double& total_ll_one_denovo);
  virtual ~DenovoResult();

  const int& get_phenotype() const {return phenotype_;}
  const double& get_posterior() const {return posterior_;}
  const std::string& get_child_id() const {return child_id_;}
  
 private:
  void CalculatePosterior();
  std::string family_id_;
  std::string child_id_;
  int phenotype_;
  double total_ll_no_mutation_;
  double total_ll_one_denovo_;
  double posterior_;
};

class TrioDenovoScanner {
 private:
  static std::string START_KEY, END_KEY, PERIOD_KEY;
  PedigreeSet pedigree_set_;
  Options options_;
  ofstream locus_summary_;

 public:
  TrioDenovoScanner(const PedigreeSet& pedigree_set,
		    const Options& options)
    : pedigree_set_(pedigree_set), options_(options), locus_summary_(options.outprefix + ".locus_summary.tab") {
    locus_summary_ << "chrom\tpos\tperiod\ttotal_children\ttotal_mutations\ttotal_mutation_rate\t"
		   << "affected_children\taffected_mutations\taffected_mutation_rate\t"
		   << "unaffected_children\tunaffected_mutations\tunaffected_mutation_rate\t"
		   << "p-value\tchildren_with_mutations\n";
  }
  virtual ~TrioDenovoScanner();

  void scan(VCF::VCFReader& strvcf);
  void summarize_results(std::vector<DenovoResult>& dnr,
			 VCF::Variant& str_variant);
};


#endif
