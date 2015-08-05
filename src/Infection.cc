/*
  This file is part of the FRED system.

  Copyright (c) 2010-2015, University of Pittsburgh, John Grefenstette,
  Shawn Brown, Roni Rosenfield, Alona Fyshe, David Galloway, Nathan
  Stone, Jay DePasse, Anuroop Sriram, and Donald Burke.

  Licensed under the BSD 3-Clause license.  See the file "LICENSE" for
  more information.
*/

// Infection.cc
// ------------
// Stores the infectivity and symptomaticity trajectories that determine the transition dates for this person.
#include <stdexcept>
#include "limits.h"
#include <float.h>
#include <vector>
#include <string>
#include <sstream>

using namespace std;

#include "Infection.h"

#include "Disease.h"
#include "Global.h"
#include "Household.h"
#include "Person.h"
#include "Place.h"
#include "Health.h"
#include "Place_List.h"

#define NEVER (-1)

using std::out_of_range;

Infection::Infection(Disease* _disease, Person* _infector, Person* _host, Place* _place, int day) {
  // FRED_VERBOSE(0,"Infection constructor entered\n");

  // general
  this->disease = _disease;
  this->infector = _infector;
  this->host = _host;
  this->place = _place;
  this->infectee_count = 0;
  this->is_susceptible = true;

  // parameters
  this->infectivity_multp = 1.0;
  this->infectivity = 0.0;
  this->susceptibility = 0.0;
  this->symptoms = 0.0;

  this->asymptomatic_period = 0;
  this->symptomatic_period = 0;

  this->exposure_date = day;
  this->infectious_start_date = -1;
  this->infectious_end_date = -1;
  this->symptoms_start_date = -1;
  this->symptoms_end_date = -1;
  this->immunity_end_date = -1;

  this->will_be_symptomatic = false;
  this->infection_is_fatal_today = false;

  // Determine if this infection produces an immune response
  this->immune_response = this->disease->gen_immunity_infection(this->host->get_real_age());

  int age = this->host->get_age();
  this->trajectory = this->disease->get_trajectory(age);
  assert(this->trajectory != NULL);

  set_transition_dates();
}

void Infection::set_transition_dates() {
  bool was_latent = true;
  bool was_incubating = true;

  this->asymptomatic_period = 0;
  this->symptomatic_period = 0;

  this->infectious_start_date = NEVER;
  this->infectious_end_date = NEVER;
  this->symptoms_start_date = NEVER;
  this->symptoms_end_date = NEVER;
  this->asymptomatic_date = NEVER;

  Trajectory::iterator trj_it = Trajectory::iterator(this->trajectory);
  while(trj_it.has_next()) {
    bool infective = (trj_it.next().infectivity > this->disease->get_infectivity_threshold());
    bool symptomatic = (trj_it.next().symptomaticity > this->disease->get_symptomaticity_threshold());
    bool asymptomatic = (infective && !(symptomatic));

    if(infective && was_latent) {
      // become infectious
      int latent_period = trj_it.get_current();
      this->infectious_start_date = this->exposure_date + latent_period;
      if(asymptomatic & was_latent) {
        this->asymptomatic_date = this->infectious_start_date;
      }
      was_latent = false;
    }

    if(infective) {
      this->infectious_end_date = this->exposure_date + trj_it.get_current() + 1;
    }

    if(symptomatic && was_incubating) {
      // become symptomatic
      int incubation_period = trj_it.get_current();
      this->symptoms_start_date = this->exposure_date + incubation_period;
      this->will_be_symptomatic = true;
      was_incubating = false;
    }

    if(symptomatic) {
      this->symptomatic_period++;
      this->symptoms_end_date = this->exposure_date + trj_it.get_current() + 1;
    }

    if(asymptomatic) {
      this->asymptomatic_period++;
    }
  }

  int recovery_period = this->disease->get_days_recovered();
  if(recovery_period > -1) {
    this->immunity_end_date = this->infectious_end_date + recovery_period;
  }
  else {
    this->immunity_end_date = NEVER;
  }
}

void Infection::update(int today) {

  // printf("INFECTION Day %d disease_name |%s|\n", today, this->disease->get_disease_name());
  if (strcmp(this->disease->get_disease_name(), "hiv") == 0) {
    chronic_update(today);
    return;
  }

  if(this->trajectory == NULL) {
    return;
  }

  int days_post_exposure = today - this->exposure_date;

  Trajectory::point trajectory_point = this->trajectory->get_data_point(days_post_exposure);
  this->infectivity = trajectory_point.infectivity;
  this->symptoms = trajectory_point.symptomaticity;
  
  if(today == get_infectious_start_date()) {
    this->host->become_infectious(this->disease);
  }

  if(today == get_symptoms_start_date()) {
    this->host->become_symptomatic(this->disease);
  }

  if(today == get_symptoms_end_date()) {
    this->host->become_asymptomatic(this->disease);
  }

  if(today == get_infectious_end_date()) {
    this->host->recover(this->disease);
  }

  if(today == get_unsusceptible_date()) {
    this->host->become_unsusceptible(this->disease);
    this->is_susceptible = false;
  }

  if(today != get_infectious_end_date()) {
  std:vector<int> strains;
    this->trajectory->get_all_strains(strains);
  }

  // if host is symptomatic, determine if infection is fatal today.
  // if so, set flag and terminate infection update.
  if(this->disease->is_case_fatality_enabled() && is_symptomatic()) {
    int days_symptomatic = today - this->symptoms_start_date;
    if(Global::Enable_Chronic_Condition) {
      if(this->disease->is_fatal(this->host, this->symptoms, days_symptomatic)) {
        set_fatal_infection();
        return;
      }
    } else {
      if(this->disease->is_fatal(this->host->get_real_age(), this->symptoms, days_symptomatic)) {
        set_fatal_infection();
        return;
      }
    }
  }
}

void Infection::chronic_update(int today) {
  int days_post_exposure = today - this->exposure_date;
  // printf("CHRONIC day %d post %d\n", today, days_post_exposure);
  if (days_post_exposure > 3 && this->host->is_infectious(disease->get_id()) == false) {
    this->host->become_infectious(this->disease);
  }
}


bool Infection::is_infectious() const {
  return this->infectivity > this->disease->get_infectivity_threshold();
}

bool Infection::is_symptomatic() const {
  return this->symptoms > this->disease->get_symptomaticity_threshold();
}


void Infection::print() const {
  printf("INF: Infection of disease type: %i in person %i "
	 "dates: exposed: %i, infectious: %i, symptomatic: %i, recovered: %i susceptible: %i "
	 "will have symp? %i, suscept: %.3f infectivity: %.3f "
	 "infectivity_multp: %.3f symptms: %.3f\n", this->disease->get_id(), this->host->get_id(),
	 this->exposure_date, get_infectious_start_date(), get_symptoms_start_date(),
	 get_infectious_end_date(), get_immunity_end_date(), this->will_be_symptomatic, this->susceptibility,
	 this->infectivity, this->infectivity_multp, this->symptoms);
}

void Infection::setTrajectory(Trajectory* _trajectory) {
  this->trajectory = _trajectory;
  set_transition_dates();
}

void Infection::report_infection(int day) const {
  if(Global::Infectionfp == NULL) {
    return;
  }

  int place_id = (this->place == NULL ? -1 : this->place->get_id());
  char place_type = (this->place == NULL ? 'X' : this->place->get_type());
  char place_subtype = 'X';
  if(this->place != NULL && this->place->is_group_quarters()) {
    if(this->place->is_college()) {
      place_subtype = 'D';
    }
    if(this->place->is_prison()) {
      place_subtype = 'J';
    }
    if(this->place->is_nursing_home()) {
      place_subtype = 'L';
    }
    if(this->place->is_military_base()) {
      place_subtype = 'B';
    }
  }
  int place_size = (this->place == NULL ? -1 : this->place->get_container_size());
  std::stringstream infStrS;
  infStrS.precision(3);
  infStrS << fixed << "day " << day << " dis " << this->disease->get_id() << " host " << this->host->get_id()
	  << " age " << this->host->get_real_age() << " sick_leave " << this->host->is_sick_leave_available()
	  << " infector " << (this->infector == NULL ? -1 : this->infector->get_id()) << " inf_age "
	  << (this->infector == NULL ? -1 : this->infector->get_real_age()) << " inf_sympt "
	  << (this->infector == NULL ? -1 : this->infector->is_symptomatic()) << " inf_sick_leave "
	  << (this->infector == NULL ? -1 : this->infector->is_sick_leave_available())
	  << " at " << place_type << " place " <<  place_id << " subtype " << place_subtype;
  infStrS << " size " << place_size << " is_teacher " << (int)this->host->is_teacher();

  if (place_type != 'X') {
    fred::geo lat = this->place->get_latitude();
    fred::geo lon = this->place->get_longitude();
    infStrS << " lat " << lat;
    infStrS << " lon " << lon;
  }
  else {
    infStrS << " lat " << -999;
    infStrS << " lon " << -999;
  }
  double host_lat = this->host->get_household()->get_latitude();
  double host_lon = this->host->get_household()->get_longitude();
  infStrS << " home_lat " << host_lat;
  infStrS << " home_lon " << host_lon;
  infStrS << " infector_exp_date " << (this->infector == NULL ? -1 : this->infector->get_exposure_date(disease->get_id()));
  infStrS << " | DATES exp " << this->exposure_date << " inf " << get_infectious_start_date() << " symp "
	  << get_symptoms_start_date() << " rec " << get_infectious_end_date() << " sus "
	  << get_immunity_end_date();

  if(Global::Track_infection_events > 1) {
    if(place_type != 'X'  && infector != NULL) {
      double host_x = this->host->get_x();
      double host_y = this->host->get_y();
      double infector_x = this->infector->get_x();
      double infector_y = this->infector->get_y();
      double distance = sqrt((host_x-infector_x)*(host_x-infector_x) +
			     (host_y-infector_y)*(host_y-infector_y));
      infStrS << " dist " << distance;
    } else {
      infStrS << " dist -1 ";
    }
    //Add Census Tract information. If there was no infector, censustract is -1
    if(this->infector == NULL) {
      infStrS << " census_tract -1";
    } else {

      Household* hh = static_cast<Household*>(this->infector->get_household());
      if(hh == NULL) {
        if(Global::Enable_Hospitals && this->infector->is_hospitalized() && this->infector->get_permanent_household() != NULL) {
          hh = static_cast<Household*>(this->infector->get_permanent_household());;
        }
      }
      int census_tract_index = (hh == NULL ? -1 : hh->get_census_tract_index());
      long int census_tract = (census_tract_index == -1 ? -1 : Global::Places.get_census_tract_with_index(census_tract_index));
      infStrS << " census_tract " << census_tract;
    }
    infStrS << " | will_be_symp? " << this->will_be_symptomatic << " sucs " << this->susceptibility
	    << " infect " << this->infectivity << " inf_multp " << this->infectivity_multp << " sympts "
	    << this->symptoms;
  }
  infStrS << "\n";

  fprintf(Global::Infectionfp, "%s", infStrS.str().c_str());
  // flush performed at the end of every day so that it doesn't gum up multithreading
  //fflush(Global::Infectionfp);
}

double Infection::get_infectivity(int day) const {
  int days_post_exposure = day - this->exposure_date;
  Trajectory::point point = this->trajectory->get_data_point(days_post_exposure);
  return point.infectivity * this->infectivity_multp;
}

double Infection::get_symptoms(int day) const {
  day = day - this->exposure_date;
  Trajectory::point point = this->trajectory->get_data_point(day);
  return point.symptomaticity;
}

////////////////////
//////////////////// methods for antivirals
////////////////////


void Infection::advance_seed_infection(int days_to_advance) {
  // only valid for seed infections!
  assert(this->infectious_end_date != -1);
  assert(this->exposure_date != -1);
  this->exposure_date -= days_to_advance;
  set_transition_dates();
  if(get_infectious_start_date() <= 0 + Global::Epidemic_offset) {
    this->host->become_infectious(this->disease);
  }
  if(get_symptoms_start_date() <= 0 + Global::Epidemic_offset) {
    this->host->become_symptomatic(this->disease);
  }
  if(get_infectious_end_date() <= 0 + Global::Epidemic_offset) {
    this->host->recover(this->disease);
  }
  if(get_unsusceptible_date() <= 0 + Global::Epidemic_offset) {
    this->host->become_unsusceptible(this->disease);
  }
}

void Infection::modify_symptomatic_period(double multp, int today) {
  // negative multiplier
  if(multp < 0)
    throw out_of_range("cannot modify: negative multiplier");

  // after symptomatic period
  if(today >= get_infectious_end_date()) {
    throw out_of_range("cannot modify: past symptomatic period");

  } else if(today < get_symptoms_start_date()) { // before symptomatic period
    this->trajectory->modify_symp_period(this->symptoms_start_date, this->symptomatic_period * multp);
    set_transition_dates();
  } else { // during symptomatic period
    // if days_left becomes 0, we make it 1 so that update() sees the new dates
    //int days_into = today - get_symptoms_start_date();
    int days_left = get_infectious_end_date() - today;
    days_left *= multp;
    this->trajectory->modify_symp_period(today - this->exposure_date, days_left);
    set_transition_dates();
  }
}

void Infection::modify_asymptomatic_period(double multp, int today) {
  // negative multiplier
  if(multp < 0) {
    throw out_of_range("cannot modify: negative multiplier");
  }

  // after asymptomatic period
  if(today >= get_symptoms_start_date()) {
    printf("ERROR: Person %d %d %d\n", host->get_id(), today, get_symptoms_start_date());
    throw out_of_range("cannot modify: past asymptomatic period");
  } else if(today < get_infectious_start_date()) { // before asymptomatic period
    this->trajectory->modify_asymp_period(this->exposure_date, this->asymptomatic_period * multp,
					  get_symptoms_start_date());
    set_transition_dates();
  } else { // during asymptomatic period
    //int days_into = today - get_infectious_start_date();
    int days_left = get_symptoms_start_date() - today;
    this->trajectory->modify_asymp_period(today - this->exposure_date, days_left * multp,
					  get_symptoms_start_date());
    set_transition_dates();
  }
}

void Infection::modify_infectious_period(double multp, int today) {
  if(today < get_symptoms_start_date()) {
    modify_asymptomatic_period(multp, today);
  }

  modify_symptomatic_period(multp, today);
}

void Infection::modify_develops_symptoms(bool symptoms, int today) {
  if((today >= get_symptoms_start_date() && get_asymptomatic_date() == -1)
     || (today >= get_infectious_end_date())) {
    throw out_of_range("cannot modify: past symptomatic period");
  }

  if(this->will_be_symptomatic != symptoms) {
    this->symptomatic_period = (this->will_be_symptomatic ? this->disease->get_days_symp() : 0);
    this->trajectory->modify_develops_symp(get_symptoms_start_date(), this->symptomatic_period);
    set_transition_dates();
  }
}

////
//// related to viral evolution
////

int Infection::get_num_past_infections() {
  return this->host->get_num_past_infections(this->disease->get_id());
}

Past_Infection* Infection::get_past_infection(int i) {
  return this->host->get_past_infection(this->disease->get_id(), i);
}

void Infection::get_strains(std::vector<int> &strains) {
  return this->trajectory->get_all_strains(strains);
}

void Infection::mutate(int old_strain, int new_strain, int day) {
  this->trajectory->mutate(old_strain, new_strain, day);
}

