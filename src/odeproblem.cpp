// Copyright (C) 2013 - 2019  Metrum Research Group
//
// This file is part of mrgsolve.
//
// mrgsolve is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// mrgsolve is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with mrgsolve.  If not, see <http://www.gnu.org/licenses/>.

/**
 * 
 * @file odeproblem.cpp
 * 
 */

#include <cmath>
#include <vector>
#include "RcppInclude.h"
#include "odeproblem.h"
#include "mrgsolve.h"

static Rcpp::NumericMatrix OMEGADEF(1,1);
static arma::mat OMGADEF(1,1,arma::fill::zeros);

//! the maximum number of iterations for steady-state calculation
#define MRGSOLVE_MAX_SS_ITER 1000

void dosimeta(void* prob_) {
  odeproblem* prob = reinterpret_cast<odeproblem*>(prob_);
  arma::mat eta = prob->mv_omega(1);
  for(unsigned int i=0; i < eta.n_cols; ++i) {
    prob->eta(i,eta(0,i)); 
  }
}

void dosimeps(void* prob_) {
  odeproblem* prob = reinterpret_cast<odeproblem*>(prob_);
  arma::mat eps = prob->mv_sigma(1);
  for(unsigned int i=0; i < eps.n_cols; ++i) {
    prob->eps(i,eps(0,i)); 
  }
}

odeproblem::odeproblem(Rcpp::NumericVector param,
                       Rcpp::NumericVector init,
                       Rcpp::List funs,
                       int n_capture_) {
  
  int npar_ = int(param.size());
  int neq_ = int(init.size());
  Neq = neq_;
  Npar = npar_;
  Istate = 1;
  
  Advan = 13;
  
  //Param = new double[npar_]();
  Param.assign(Npar,0.0);
  Y.assign(Neq,0.0);
  Yout.assign(Neq+1,0.0);
  Ydot.assign(Neq,0.0);
  Init_value.assign(Neq,0.0);
  Init_dummy.assign(Neq,0.0);
  
  R0.assign(Neq,0.0);
  infusion_count.assign(Neq,0);
  R.assign(Neq,0.0);
  D.assign(Neq,0.0);
  F.assign(Neq,1.0);
  Alag.assign(Neq,0.0);
  
  On.assign(Neq,1);
  
  d.evid = 0;
  d.newind = 0;
  d.time = 0.0;
  d.id = 1.0;
  d.EPS.assign(50,0.0);
  d.ETA.assign(50,0.0);
  d.CFONSTOP = false;
  d.cmt = 0;
  d.amt = 0;
  
  Do_Init_Calc = true;
  
  pred.assign(5,0.0);
  
  for(int i=0; i < Npar; ++i) Param[i] =       double(param[i]);
  for(int i=0; i < Neq;  ++i) Init_value[i] =  double(init[i]);
  
  *reinterpret_cast<void**>(&Inits)  = R_ExternalPtrAddr(funs["main"]);
  *reinterpret_cast<void**>(&Table)  = R_ExternalPtrAddr(funs["table"]);
  *reinterpret_cast<void**>(&Derivs) = R_ExternalPtrAddr(funs["ode"]);
  *reinterpret_cast<void**>(&Config) = R_ExternalPtrAddr(funs["config"]);
  
  Capture.assign(n_capture_,0.0);
  
  simeta = mrgsolve::resim(&dosimeta,reinterpret_cast<void*>(this));
  simeps = mrgsolve::resim(&dosimeps,reinterpret_cast<void*>(this));
  
}


/**
 @brief Destructor for odeproblem object.
 
 Upon object construction, odeproblem dynamically allocates the Param 
 array.
 
 */
odeproblem::~odeproblem(){

}

double odeproblem::fbio(unsigned int pos) {
  if(Neq==0) return 1.0;
  return F.at(pos);
}

double odeproblem::alag(int cmt){
  if(Neq==0) return 0.0;
  return Alag.at(cmt);
}


//! set number of <code>ETAs</code> in the model
void odeproblem::neta(int n) {
  if(n > 25) d.ETA.assign(n,0.0);
}

//! set number of <code>EPSs</code> in the model
void odeproblem::neps(int n) {
  if(n > 25) d.EPS.assign(n,0.0);
}

void odeproblem::tol(double atol, double rtol) {
  Atol = atol;
  Rtol = rtol;
}

/** 
 * Assigns values to both the compartment and the 
 * vector of initial conditions.
 * 
 * @param pos the compartment number (C++ indexing)
 * @param value the value for the compartment
 * 
 */
void odeproblem::y_init(int pos, double value) {
  Y[pos] = value;
  Init_value[pos] = value;
  Init_dummy[pos] = value;
}

void odeproblem::y_init(Rcpp::NumericVector x) {
  if(x.size() != Neq) Rcpp::stop("Initial vector is wrong size");
  for(int i = 0; i < x.size(); ++i) {
    Y[i] = x[i];
    Init_value[i] = x[i];
    Init_dummy[i] = x[i];
  }
}

//! add <code>value</code> to compartment <code>pos</code>
void odeproblem::y_add(const unsigned int pos, const double& value) {
  Y[pos] = Y[pos] + value; 
}


/** Derivative function that gets called by the solver. 
 * 
 * @param neq number of equations
 * @param t solver time
 * @param y current state
 * @param ydot left hand side of differential equations
 * @param prob an odeproblem object
 */
void main_derivs(double t, double *y, double *ydot, void *data) {
  odeproblem *prob = reinterpret_cast<odeproblem *>(data);
  prob->call_derivs(&(prob->Neq),&t,y,ydot);  
}

void odeproblem::call_derivs(int *neq, double *t, double *y, double *ydot) {
  Derivs(t,y,ydot,Init_value,Param);
  for(int i = 0; i < Neq; ++i) {
    ydot[i] = (ydot[i] + R0[i])*On[i]; 
  }
}


void odeproblem::set_d(rec_ptr this_rec) {
  d.time = this_rec->time();
  d.cmt = this_rec->cmt();
  d.evid = this_rec->evid();
  d.amt = this_rec->amt();
}

/**
 * Call <code>$MAIN</code> to get the initial conditions.
 * 
 * @param time the time to assume for the calculation
 */
void odeproblem::init_call(const double& time) {
  
  d.time = time;
  
  if(Do_Init_Calc) {
    Inits(Init_value,Y,Param,F,Alag,R,D,d,pred,simeta);
    for(int i=0; i < Neq; ++i) {
      Y[i] = Init_value[i];
      Init_dummy[i] = Init_value[i];
    }
  } else {
    for(int i=0; i < Neq; ++i) {
      Init_dummy[i] = Init_value[i];
    }
    Inits(Init_dummy,Y,Param,F,Alag,R,D,d,pred,simeta);
  }
}


/**
 * Call <code>$MAIN</code> with the dummy initial condition vector.
 * 
 * @param time the time to assume when making the call.
 */
void odeproblem::init_call_record(const double& time) {
  d.time = time;
  Inits(Init_dummy,Y,Param,F,Alag,R,D,d,pred,simeta);
}

//! Call <code>$TABLE</code> function.
void odeproblem::table_call() {
  Table(Y,Init_value,Param,F,R,d,pred,Capture,simeps);  
}

//! Call <code>$PREAMBLE</code> function.
void odeproblem::config_call() {
  Config(d,Param,Neq,Npar);
}

//! Reset all infusion rates.
void odeproblem::rate_reset() {
  for(int i = 0; i < Neq; ++i) {
    R0[i] = 0.0;
    infusion_count[i] = 0;
  }
}

void odeproblem::rate_main(rec_ptr rec) {
  if(rec->rate() == -1) {
    if(this->rate(rec->cmtn()) <= 0) {
      throw Rcpp::exception("Invalid infusion setting: rate (R_CMT).", false);
    }
    rec->rate(this->rate(rec->cmtn()));
  }
  if(rec->rate() == -2) {
    if(this->dur(rec->cmtn()) <= 0) {
      throw Rcpp::exception("Invalid infusion setting: duration (D_CMT).",false);
    }
    rec->rate(rec->amt() * this->fbio(rec->cmtn()) / this->dur(rec->cmtn()));
  }
}

//! Reset <code>odeproblem</code> object for new individual.
void odeproblem::reset_newid(const double id_) {
  
  for(int i = 0; i < Neq; ++i) {
    R0[i] = 0.0;
    R[i] = 0.0;
    D[i] = 0.0;
    infusion_count[i] = 0;
    On[i] = 1;
    F[i] = 1.0;
    Alag[i] = 0;
  }
  
  d.newind = 1;
  d.time = 0.0;
  
  d.SYSTEMOFF=0;
  this->lsoda_init();
  d.id = id_;
}

void odeproblem::rate_add(const unsigned int pos, const double& value) {
  ++infusion_count[pos];
  R0[pos] = R0[pos] + value;
}

void odeproblem::rate_bump(const unsigned int pos, const double& value) {
  R0[pos] = R0[pos] + value;
}

void odeproblem::rate_rm(const unsigned int pos, const double& value) {
  if(infusion_count[pos] <= 0){
    infusion_count[pos] = 0;
    R0[pos] = 0.0;
    return;
  } else {
    --infusion_count[pos];
    R0[pos] = R0[pos] - value;
    if(R0[pos] < 0.0) R0[pos] = 0.0;
  }
}

void odeproblem::on(const unsigned short int eq_n) {
  On[eq_n] = 1;
}

void odeproblem::off(const unsigned short int eq_n) {
  if(infusion_count[eq_n]>0) {
    Rcpp::stop("Attempting to turn compartment off when infusion is on.");
  }
  On[eq_n] = 0;
  this->y(eq_n,0.0);
}


void odeproblem::advance(double tfrom, double tto, LSODA& solver) {
  
  if(Neq == 0) return;
  
  if(Advan != 13) {
    if((Advan==2) | (Advan==1)) {
      this->advan2(tfrom,tto);
      return;
    }
    
    if((Advan==4) | (Advan==3)) {
      this->advan4(tfrom,tto);
      return;
    }
    // If Advan isn't 13, it needs to be 0/1/2/3/4
    Rcpp::stop("mrgsolve: advan has invalid value.");
  }
  // void LSODA::lsoda_update(LSODA_ODE_SYSTEM_TYPE f, 
  //                        const int neq,
  //                        vector<double>& y, 
  //                        vector<double>& yout, 
  //                        double *t,
  //                        const double tout, 
  //                        int *istate,
  //                        void *_data)
  solver.lsoda_update(main_derivs,Neq,Y,Yout,&tfrom,tto,&Istate,(void*) this);

  //Rcpp::Rcout << " TRying to advance" << std::endl;
  //this->call_derivs(&Neq, &tto, Y, Ydot);
}

void odeproblem::advan2(const double& tfrom, const double& tto) {
  
  unsigned int neq = this->neq();
  
  double dt = tto-tfrom;
  
  if(MRGSOLVE_GET_PRED_CL <= 0) Rcpp::stop("pred_CL has a 0 or negative value.");
  if(MRGSOLVE_GET_PRED_VC <= 0) Rcpp::stop("pred_VC has a 0 or negative value.");
  
  double k10 = MRGSOLVE_GET_PRED_K10;
  double ka =  MRGSOLVE_GET_PRED_KA;
  
  if(k10 <= 0) Rcpp::stop("k10 has a 0 or negative value");
  
  //a and alpha are private members
  alpha[0] = k10;
  alpha[1] = ka;
  
  a[0] = ka/(ka-alpha[0]);
  a[1] = -a[0];
  
  double init0 = 0, init1 = 0;
  int eqoffset = 0;
  
  if(neq==1) {
    init0 = 0;
    init1 = this->y(0);
    eqoffset = 1;
  }
  if(neq==2) {
    init0 = this->y(0);
    init1 = this->y(1);
  }
  
  double pred0 = 0, pred1 = 0;
  
  if(neq ==2) {
    if((init0!=0) || (R0[0]!=0)) {
      
      pred0 = init0*exp(-ka*dt);//+ R0[0]*(1-exp(-ka*dt))/ka;
      
      if(ka > 0) { // new
        pred0 += R0[0]*(1.0-exp(-ka*dt))/ka; // new
        pred1 +=
          PolyExp(dt,init0,0.0  ,0.0,0.0,false,a,alpha,2) +
          PolyExp(dt,0.0  ,R0[0],dt ,0.0,false,a,alpha,2);
      } else {
        pred0 += R0[0]*dt; // new
      }
    }
  }
  
  if((init1!=0) || (R0[1-eqoffset]!=0)) {
    a[0] = 1;
    pred1 +=
      PolyExp(dt,init1,0.0  ,0.0,0.0,false,a,alpha,1) +
      PolyExp(dt,0.0  ,R0[1-eqoffset],dt ,0.0,false,a,alpha,1);
  }
  
  if(neq==2) {
    this->y(0,pred0);
    this->y(1,pred1);
  }
  if(neq==1) {
    this->y(0,pred1);
  }
}


void odeproblem::advan4(const double& tfrom, const double& tto) {
  
  double dt = tto - tfrom;
  
  unsigned int neq = this->neq();
  
  // Make sure parameters are valid
  if (MRGSOLVE_GET_PRED_VC <=  0) Rcpp::stop("pred_VC has a 0 or negative  value.");
  if (MRGSOLVE_GET_PRED_VP <=  0) Rcpp::stop("pred_VP has a 0 or negative  value.");
  if (MRGSOLVE_GET_PRED_Q  <   0) Rcpp::stop("pred_Q has a  negative  value.");
  if (MRGSOLVE_GET_PRED_CL <=  0) Rcpp::stop("pred_CL has a 0 or negative  value.");
  
  double ka =  MRGSOLVE_GET_PRED_KA;
  double k10 = MRGSOLVE_GET_PRED_K10;
  double k12 = MRGSOLVE_GET_PRED_K12;
  double k21 = MRGSOLVE_GET_PRED_K21;
  
  double ksum = k10+k12+k21;
  
  double init0 = 0, init1 = 0, init2 = 0,  pred0 = 0, pred1 = 0, pred2 = 0;
  
  int eqoffset = 0;
  
  if(neq == 2) {
    init0 = 0; init1 = this->y(0); init2 = this->y(1);
    eqoffset = 1;
  }
  if(neq ==3) {
    init0 = this->y(0); init1 = this->y(1); init2 = this->y(2);
  }
  
  //a and alpha are private members
  
  alpha[0] = (ksum + sqrt(ksum*ksum-4.0*k10*k21))/2.0;
  alpha[1] = (ksum - sqrt(ksum*ksum-4.0*k10*k21))/2.0;
  alpha[2] = ka;
  
  if(neq==3) { // only do the absorption compartment if we have 3
    if((init0 != 0) || (R0[0] != 0)) {
      
      pred0 = init0*exp(-ka*dt);// + R0[0]*(1.0-exp(-ka*dt))/ka;
      
      a[0] = ka*(k21-alpha[0])/((ka-alpha[0])*(alpha[1]-alpha[0]));
      a[1] = ka*(k21-alpha[1])/((ka-alpha[1])*(alpha[0]-alpha[1]));
      a[2] = -(a[0]+a[1]);
      
      if(ka > 0) {  // new
        pred0 += R0[0]*(1.0-exp(-ka*dt))/ka; // new
        pred1 +=
          PolyExp(dt,init0,0,0,0,false,a,alpha,3) +
          PolyExp(dt,0,R0[0],dt,0,false,a,alpha,3);
        
        a[0] = ka * k12/((ka-alpha[0])*(alpha[1]-alpha[0]));
        a[1] = ka * k12/((ka-alpha[1])*(alpha[0]-alpha[1]));
        a[2] = -(a[0] + a[1]);
        
        pred2 +=
          PolyExp(dt,init0,0,0,0,false,a,alpha,3) +
          PolyExp(dt,0,R0[0],dt,0,false,a,alpha,3);
      } else {
        pred0 += R0[0]*dt; // new
      }
    }
  }
  
  if((init1 != 0) || (R0[1-eqoffset] != 0)) {
    
    a[0] = (k21 - alpha[0])/(alpha[1]-alpha[0]) ;
    a[1] = (k21 - alpha[1])/(alpha[0]-alpha[1]) ;
    
    pred1 +=
      PolyExp(dt,init1,0,0,0,false,a,alpha,2) +
      PolyExp(dt,0,R0[1-eqoffset],dt,0,false,a,alpha,2);
    
    a[0] = k12/(alpha[1]-alpha[0]) ;
    a[1] = -a[0];
    
    pred2 +=
      PolyExp(dt,init1,0,0,0,false,a,alpha,2) +
      PolyExp(dt,0,R0[1-eqoffset],dt,0,false,a,alpha,2);
  }
  
  if((init2 != 0) || (R0[2-eqoffset] != 0)) {
    
    a[0] = k21/(alpha[1]-alpha[0]);
    a[1] = -a[0];
    
    pred1 +=
      PolyExp(dt,init2,0,0,0,false,a,alpha,2) +
      PolyExp(dt,0,R0[2-eqoffset],dt,0,false,a,alpha,2);
    
    a[0] = (k10 + k12 - alpha[0])/(alpha[1]-alpha[0]);
    a[1] = (k10 + k12 - alpha[1])/(alpha[0]-alpha[1]);
    
    pred2 +=
      PolyExp(dt,init2,0,0,0,false,a,alpha,2) +
      PolyExp(dt,0,R0[2-eqoffset],dt,0,false,a,alpha,2);
  }
  
  if(neq ==2) {
    this->y(0,pred1);
    this->y(1,pred2);
  }
  if(neq ==3) {
    this->y(0,pred0);
    this->y(1,pred1);
    this->y(2,pred2);
  }
}


double PolyExp(const double& x,
               const double& dose,
               const double& rate,
               const double& xinf,
               const double& tau,
               const bool ss,
               const dvec& a,
               const dvec& alpha,
               const int n) {
  
  double result=0, bolusResult, dx, nlntv ;
  double inf=1E9;
  //maximum value for a double in C++
  int i;
  
  //assert((alpha.size() >= n) && (a.size() >= n));
  
  //UPDATE DOSE
  if (dose>0) {
    if((tau<=0)&&(x>=0)) {
      for(i=0;i<n; ++i){result += a[i]*exp(-alpha[i]*x);}
    }
    else if(!ss)  {
      nlntv=x/tau+1;
      dx=x-trunc(x/tau)*tau; // RISKY, but preliminary results suggest that trunc
      // works on Stan variables.
      for(i=0;i<n;++i) {
        result += a[i]*exp(-alpha[i]*x)
        *(1-exp(-nlntv*alpha[i]*tau))/(1-exp(-alpha[i]*tau));
      }
    }
    
    else {
      dx = x-trunc(x/tau)*tau;
      for(i=0;i<n;++i) result += a[i]*exp(-alpha[i]*x)/(1-exp(-alpha[i]*tau));
    }
  }
  bolusResult = dose*result;
  
  //UPDATE RATE
  result=0;
  if((rate>0)&&(xinf<inf)) {
    if(tau<=0) {
      if(x>=0) {
        if(x<=xinf) {
          for(i=0;i<n;++i) result += a[i]*(1-exp(-alpha[i]*x))/alpha[i];
        }
        else {
          for(i=0;i<n;++i) {
            result += a[i]*(1-exp(-alpha[i]*xinf))*exp(-alpha[i]*(x-xinf))/alpha[i];
          }
        }
      }
    }
    
    else if(!ss) {
      if(xinf <= tau) Rcpp::stop("xinf <= tau in PolyExp");
      //assert(xinf <= tau); //and other case later, says Bill
      dx=x-trunc(x/tau)*tau;
      nlntv=trunc(x/tau)+1;
      if(dx<=xinf) {
        for(i=0;i<n;++i) {
          if(n>1) {
            result += a[i]*(1-exp(-alpha[i]*xinf))*exp(-alpha[i]*(dx-xinf+tau))
            * (1-exp(-(nlntv-1)*alpha[i]*tau))/(1-exp(-alpha[i]*tau))/alpha[i];
          }
          result += a[i]*(1-exp(-alpha[i]*dx))/alpha[i];
        }
      }
      else  {
        for(i=0;i<n;++i) {
          result += a[i] * (1 - exp(-alpha[i]*xinf))*exp(-alpha[i]*(dx-xinf)) *
            (1-exp(-nlntv*alpha[i]*tau))/(1-exp(-alpha[i]*tau)) / alpha[i];
        }
      }
    }
    
    else {
      if(xinf <= tau) Rcpp::stop("xinf <= tau in PolyExp");
      dx = x - trunc(x/tau)*tau;
      nlntv = trunc(x/tau)+1;
      if (dx <= xinf) {
        for(i=0;i<n;++i) {
          result += a[i] * (1 - exp(-alpha[i]*xinf))*exp(-alpha[i]*(dx-xinf+tau)) /
            (1-exp(-alpha[i]*tau)) / alpha[i] + a[i] * (1 - exp(-alpha[i]*dx)) / alpha[i];
        }
      }
      else {
        for(i=0;i<n;++i) {
          result += a[i] * (1 - exp(-alpha[i]*xinf))*exp(-alpha[i]*(dx-xinf)) / (1-exp(-alpha[i]*tau)) / alpha[i];
        }
      }
    }
  }
  
  else  {
    if(!ss) {
      if(x>=0) {
        for(i=0;i<n;++i) result +=a[i]*(1-exp(-alpha[i]*x))/alpha[i];
      }
    }
    else {
      for(i=0;i<n;++i) result += a[i]/alpha[i];
    }
  }
  return bolusResult + rate*result;
}

void odeproblem::copy_parin(const Rcpp::List& parin) {
  //this->tol);
  // this->hmax(Rcpp::as<double>(parin["hmax"]));
  // this->maxsteps(Rcpp::as<double>  (parin["maxsteps"]));
  // this->ixpr(Rcpp::as<double>  (parin["ixpr"]));
  // this->mxhnil(Rcpp::as<double>  (parin["mxhnil"]));
  Atol = Rcpp::as<double>(parin["atol"]); 
  Rtol = Rcpp::as<double>(parin["rtol"]);
  Hmax = Rcpp::as<double>(parin["hmax"]);
  Maxsteps = Rcpp::as<double>  (parin["maxsteps"]);
  Ixpr = Rcpp::as<double>  (parin["ixpr"]);
  Mxhnil = Rcpp::as<double>  (parin["mxhnil"]);
  Advan = Rcpp::as<int>(parin["advan"]);
  Do_Init_Calc = Rcpp::as<bool>(parin["do_init_calc"]);
}

void odeproblem::copy_funs(const Rcpp::List& funs) {
  *reinterpret_cast<void**>(&Inits)  = R_ExternalPtrAddr(funs["main"]);
  *reinterpret_cast<void**>(&Table)  = R_ExternalPtrAddr(funs["table"]);
  *reinterpret_cast<void**>(&Derivs) = R_ExternalPtrAddr(funs["ode"]);
  *reinterpret_cast<void**>(&Config) = R_ExternalPtrAddr(funs["config"]);
}

void odeproblem::advan(int x) {
  Advan = x;
  
  if(Advan==13) return;
  
  if((x==1) | (x ==2)) {
    a.assign(2,0.0);
    alpha.assign(2,0.0);
  }
  
  if((x==3) | (x==4)) {
    a.assign(3,0.0);
    alpha.assign(3,0.0);
  }
}

/**
 * Call the <code>$MAIN</code> function from a model object.
 * 
 * @param lparam model parameters
 * @param linit model initial contitions
 * @param Neta number of rows in <code>OMEGA</code>
 * @param Neps number of rows in <code>SIGMA</code>
 * @param capture vector of capture names
 * @param funs a list of model function pointers
 * @return list with updated initial conditions, number of paramerters,
 * and number of equations
 * 
 */
// [[Rcpp::export]]
Rcpp::List TOUCH_FUNS(const Rcpp::NumericVector& lparam, 
                      const Rcpp::NumericVector& linit,
                      int Neta, int Neps,
                      const Rcpp::CharacterVector& capture,
                      const Rcpp::List& funs,
                      Rcpp::Environment envir) {
  
  Rcpp::List ans;
  
  odeproblem prob(lparam, linit, funs, capture.size());
  prob.neta(Neta);
  prob.neps(Neps);
  prob.pass_envir(&envir);
  prob.newind(0);
  
  prob.config_call();
  prob.init_call(0.0);
  
  Rcpp::NumericVector init_val(linit.size());
  
  for(int i=0; i < (prob.neq()); ++i) {
    init_val[i] = prob.init(i);
  }
  
  ans["init"] = init_val;
  ans["npar"] = prob.npar();
  ans["neq"] = prob.neq();
  
  return(ans);
}

void odeproblem::omega(Rcpp::NumericMatrix& x) {
  Omega = Rcpp::as<arma::mat>(x);
}

void odeproblem::sigma(Rcpp::NumericMatrix& x) {
  Sigma = Rcpp::as<arma::mat>(x);
}

arma::mat odeproblem::mv_omega(int n) {
  return MVGAUSS(Omega,n);
}

arma::mat odeproblem::mv_sigma(int n) {
  return MVGAUSS(Sigma,n);
}

