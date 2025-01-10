/*  File src/wtmodel.c in package ergm, part of the
 *  Statnet suite of packages for network analysis, https://statnet.org .
 *
 *  This software is distributed under the GPL-3 license.  It is free,
 *  open source, and has the attribution requirements (GPL Section 7) at
 *  https://statnet.org/attribution .
 *
 *  Copyright 2003-2025 Statnet Commons
 */
#include <string.h>
#include "ergm_wtmodel.h"
#include "ergm_omp.h"
#include "ergm_util.h"

void OnWtNetworkEdgeChangeUWrap(Vertex tail, Vertex head, double weight, void *mtp, WtNetwork *nwp, double edgestate){
  ((WtModelTerm *) mtp)->u_func(tail, head, weight, mtp, nwp, edgestate);
}

/*
  WtInitStats
  A helper's helper function to initialize storage for functions that use it.
*/
static inline void WtInitStats(WtNetwork *nwp, WtModel *m){

  /* This function must do things in very specific order:

   1. Since dependent terms go before the dependency, the
      initialization must be performed in reverse order.

   2. Since a dependt term relies on the pre-toggle state of the
      dependency, the updating must be performed in forward order.

   3. Since an i_function can choose to delete its own u_function, we
      can't add callbacks until after the i_functions have been
      called.

   4. Some terms may have subterms that add callbacks to the same
      network, and the subterms' u_functions must be called *after*
      the u_functions of the terms that depend on them (per rule 2).

   Therefore, this code first stores the current position in the
   callback list, then initializes the terms, then adds callbacks in
   front of the new callbacks, i.e., at the then-current
   position. Repeatedly adding the terms at that position in reverse
   order is slightly inefficient, but it has to be done infrequently.

   */
  unsigned int on_edge_change_pos = nwp->n_on_edge_change; // Save the current position.

  // Iterate in reverse, so that auxliary terms get initialized first.
  WtEXEC_THROUGH_TERMS_INREVERSE(m, {
      if(!m->noinit_s || !mtp->s_func){ // Skip if noinit_s is set and s_func is present.
        double *dstats = mtp->dstats;
        mtp->dstats = NULL; // Trigger segfault if i_func tries to write to change statistics.
        if(mtp->i_func)
          (*(mtp->i_func))(mtp, nwp);  /* Call i_??? function */
        else if(mtp->u_func) /* No initializer but an updater -> uses a 1-function implementation. */
          (*(mtp->u_func))(0, 0, 0, mtp, nwp, 0);  /* Call u_??? function */
        mtp->dstats = dstats;
      }
      // Now, bind the term to the network through the callback API.
      if(mtp->u_func && (!m->noinit_s || !mtp->s_func)) // Skip if noinit_s is set and s_func is present.
        AddOnWtNetworkEdgeChange(nwp, OnWtNetworkEdgeChangeUWrap, mtp, on_edge_change_pos);
    });
}

/*
  WtDestroyStats
  A helper's helper function to finalize storage for functions that use it.
*/
static inline void WtDestroyStats(WtNetwork *nwp, WtModel *m){
  unsigned int i=0;
  WtEXEC_THROUGH_TERMS(m, {
      if(!m->noinit_s || !mtp->s_func){ // Skip if noinit_s is set and s_func is present.
        if(mtp->u_func)
          DeleteOnWtNetworkEdgeChange(nwp, OnWtNetworkEdgeChangeUWrap, mtp);
        if(mtp->f_func)
          (*(mtp->f_func))(mtp, nwp);  /* Call f_??? function */
      }
      R_Free(m->dstatarray[i]);
      R_Free(mtp->statcache);
      if(mtp->storage){
        R_Free(mtp->storage);
        mtp->storage = NULL;
      }
      i++;
    });
}

/*****************
  void WtModelDestroy
******************/
void WtModelDestroy(WtNetwork *nwp, WtModel *m)
{  
  WtDestroyStats(nwp, m);

  for(unsigned int i=0; i < m->n_aux; i++)
    if(m->termarray[0].aux_storage[i]!=NULL){
      R_Free(m->termarray[0].aux_storage[i]);
      m->termarray[0].aux_storage[i] = NULL;
  }
  
  if(m->n_terms && m->termarray[0].aux_storage!=NULL){
    R_Free(m->termarray[0].aux_storage);
  }
  
  WtEXEC_THROUGH_TERMS(m, {
      if(mtp->aux_storage!=NULL)
	mtp->aux_storage=NULL;
    });
  
  R_Free(m->dstatarray);
  R_Free(m->termarray);
  R_Free(m->workspace_backup);
  R_Free(m);
}

/*****************
 int WtModelInitialize

 Allocate and initialize the WtModelTerm structures, each of which contains
 all necessary information about how to compute one term in the model.
*****************/
WtModel* WtModelInitialize (SEXP mR, SEXP ext_state, WtNetwork *nwp, Rboolean noinit_s) {
  SEXP terms = getListElement(mR, "terms");
  if(ext_state == R_NilValue) ext_state = NULL;

  WtModel *m = (WtModel *) R_Calloc(1, WtModel);
  unsigned int n_terms = m->n_terms = length(terms);
  m->termarray = (WtModelTerm *) R_Calloc(n_terms, WtModelTerm);
  m->dstatarray = (double **) R_Calloc(n_terms, double *);
  m->n_stats = 0;
  m->n_aux = 0;
  m->n_u = 0;
  m->noinit_s = noinit_s;
  m->R = mR;
  m->ext_state = ext_state;
  for (unsigned int l=0; l < m->n_terms; l++) {
    WtModelTerm *thisterm = m->termarray + l;
    thisterm->R = VECTOR_ELT(terms, l);

      /* Initialize storage and term functions to NULL. */
      thisterm->storage = NULL;
      thisterm->aux_storage = NULL;
      thisterm->ext_state = NULL;
      thisterm->d_func = NULL;
      thisterm->c_func = NULL;
      thisterm->s_func = NULL;
      thisterm->i_func = NULL;
      thisterm->u_func = NULL;
      thisterm->f_func = NULL;
      thisterm->w_func = NULL;
      thisterm->x_func = NULL;
      
      /* First, obtain the term name and library: fnames points to a
      single character string, consisting of the names of the selected
      options concatenated together and separated by spaces.  This is
      passed by the calling R function.  These names are matched with
      their respective C functions that calculate the appropriate
      statistics.  Similarly, sonames points to a character string
      containing the names of the shared object files associated with
      the respective functions.*/
      const char *fname = FIRSTCHAR(getListElement(thisterm->R, "name")),
        *sn = FIRSTCHAR(getListElement(thisterm->R, "pkgname"));

      /* Check if the package is compiled against a different version of 'ergm'. */
      warn_API_version(sn);

      /* Extract the required string information from the relevant sources */
      char *fn = R_Calloc(strlen(fname)+3, char);
      fn[1]='_';
      strcpy(fn+2, fname);
      /* fn is now the string ' _[name]', where [name] is fname */

      /* Extract the term inputs. */

      /* Double input vector with an optional attribute shift. */
      SEXP tmp = getListElement(thisterm->R, "inputs");
      thisterm->ninputparams = length(tmp);
      thisterm->inputparams = thisterm->ninputparams ? REAL(tmp) : NULL;

      tmp = getAttrib(tmp, install("ParamsBeforeCov"));
      unsigned int offset = length(tmp) ? asInteger(tmp): 0;  /* Set offset for attr vector */
      thisterm->attrib = thisterm->ninputparams ? thisterm->inputparams + offset : NULL; /* Ptr to attributes */

      /* Integer input vector with an optional attribute shift. */
      tmp = getListElement(thisterm->R, "iinputs");
      thisterm->niinputparams = length(tmp);
      thisterm->iinputparams = thisterm->niinputparams ? INTEGER(tmp) : NULL;

      tmp = getAttrib(tmp, install("ParamsBeforeCov"));
      offset = length(tmp) ? asInteger(tmp): 0;  /* Set offset for attr vector */
      thisterm->iattrib = thisterm->niinputparams ? thisterm->iinputparams + offset : NULL; /* Ptr to attributes */

      /* Number of statistics. */
      thisterm->nstats = length(getListElement(thisterm->R, "coef.names")); /* If >0, # of statistics returned. If ==0 an auxiliary statistic. */

      /* Set auxiliary counts and values. */
      tmp = getAttrib(thisterm->R, install("aux.slots"));
      thisterm->n_aux = length(tmp);
      thisterm->aux_slots = (unsigned int *) INTEGER(tmp);

      /* Empty network statistics. */
      tmp = getListElement(thisterm->R, "emptynwstats");
      thisterm->emptynwstats = isNULL(tmp) ? NULL : REAL(tmp);

      /*  Update the running total of statistics */
      m->n_stats += thisterm->nstats; 
      m->dstatarray[l] = (double *) R_Calloc(thisterm->nstats, double);
      thisterm->dstats = m->dstatarray[l];  /* This line is important for
                                               eventually freeing up allocated
					       memory, since thisterm->dstats
					       can be modified but 
					       m->dstatarray[l] cannot be.  */
      thisterm->statcache = (double *) R_Calloc(thisterm->nstats, double);

      if(ext_state) thisterm->ext_state = VECTOR_ELT(ext_state, l);

      /* If the term's nstats==0, it is auxiliary: it does not affect
	 acceptance probabilities or contribute any
	 statistics. Therefore, its d_ and s_ functions are never
	 called and are not initialized. It is only used for its u_
	 function. Therefore, the following code is only run when
	 thisterm->nstats>0. */
      if(thisterm->nstats){
	/*  Most important part of the WtModelTerm:  A pointer to a
	    function that will compute the change in the network statistic of 
	    interest for a particular edge toggle.  This function is obtained by
	    searching for symbols associated with the object file with prefix
	    sn, having the name fn.  Assuming that one is found, we're golden.*/ 
	fn[0]='c';
	thisterm->c_func = 
	  (void (*)(Vertex, Vertex, double, WtModelTerm*, WtNetwork*, double))
	  R_FindSymbol(fn,sn,NULL);

        fn[0]='d';
        thisterm->d_func =
          (void (*)(Edge, Vertex*, Vertex*, double*, WtModelTerm*, WtNetwork*))
          R_FindSymbol(fn,sn,NULL);
	
        if(thisterm->c_func==NULL && thisterm->d_func==NULL){
          error("Error in WtModelInitialize: term with functions %s::%s is declared to have statistics but does not appear to have a change or a difference function. Memory has not been deallocated, so restart R sometime soon.\n",sn,fn+2);
	}
	
	/* Optional function to compute the statistic of interest for
	   the network given. It can be more efficient than going one
	   edge at a time. */
	fn[0]='s';
	thisterm->s_func = 
	  (void (*)(WtModelTerm*, WtNetwork*)) R_FindSymbol(fn,sn,NULL);

	/* Optional function to compute the statistic of interest for
	   the empty network (over and above the constant value if
	   given) and taking into account the extended state. */
	fn[0]='z';
	thisterm->z_func =
	  (void (*)(WtModelTerm*, WtNetwork*, Rboolean)) R_FindSymbol(fn,sn,NULL);
      }else m->n_aux++;
      
      /* Optional functions to store persistent information about the
	 network state between calls to d_ functions. */
      fn[0]='u';
      if((thisterm->u_func = 
	  (void (*)(Vertex, Vertex, double, WtModelTerm*, WtNetwork*, double)) R_FindSymbol(fn,sn,NULL))!=NULL) m->n_u++;

      /* Optional-optional functions to initialize and finalize the
	 term's storage, and the "eXtension" function to allow an
	 arbitrary "signal" to be sent to a statistic. */
      
      fn[0]='i';
      thisterm->i_func = 
	(void (*)(WtModelTerm*, WtNetwork*)) R_FindSymbol(fn,sn,NULL);

      fn[0]='f';
      thisterm->f_func = 
	(void (*)(WtModelTerm*, WtNetwork*)) R_FindSymbol(fn,sn,NULL);

      /* If it's an auxiliary, then it needs an i_function or a
	 u_function, or it's not doing anything. */
      if(thisterm->nstats==0 && (thisterm->i_func==NULL && thisterm->u_func==NULL)){
          error("Error in WtModelInitialize: term with functions %s::%s is declared to have no statistics but does not appear to have an updater function, so does not do anything. Memory has not been deallocated, so restart R sometime soon.\n",sn,fn+2);
      }
  
      fn[0]='w';
      thisterm->w_func =
	(SEXP (*)(WtModelTerm*, WtNetwork*)) R_FindSymbol(fn,sn,NULL);

      fn[0]='x';
      thisterm->x_func =
	(void (*)(unsigned int type, void *data, WtModelTerm*, WtNetwork*)) R_FindSymbol(fn,sn,NULL);

      if(!ext_state && (thisterm->w_func)) error("Error in ModelInitialize: not provided with extended state, but model terms with functions %s::%s requires extended state. This should normally be caught sooner. This limitation may be removed in the future.  Memory has not been deallocated, so restart R sometime soon.\n",sn,fn+2);

      /*Clean up by freeing fn*/
      R_Free(fn);
  }
  
  m->workspace_backup = m->workspace = (double *) R_Calloc(m->n_stats, double);

  unsigned int pos = 0;
  WtFOR_EACH_TERM(m){
    mtp->statspos = pos;
    pos += mtp->nstats;
  }
  
  /* Allocate auxiliary storage and put a pointer to it on every model term. */
  if(m->n_aux){
    m->termarray[0].aux_storage = (void *) R_Calloc(m->n_aux, void *);
    for(unsigned int l=1; l < n_terms; l++)
      m->termarray[l].aux_storage = m->termarray[0].aux_storage;
  }

  /* Trigger initial storage update */
  WtInitStats(nwp, m);

  /* Now, check that no term exports both a d_ and a c_
     function. TODO: provide an informative "traceback" to which term
     caused the problem.*/
  WtFOR_EACH_TERM(m){
    if(mtp->c_func && mtp->d_func) error("A term exports both a change and a difference function.  Memory has not been deallocated, so restart R sometime soon.\n");
  }

  return m;
}

void WtChangeStatsDo(unsigned int ntoggles, Vertex *tails, Vertex *heads, double *weights,
                   WtNetwork *nwp, WtModel *m){
  memset(m->workspace, 0, m->n_stats*sizeof(double)); /* Zero all change stats. */ 

  /* Make a pass through terms with d_functions. */
  WtEXEC_THROUGH_TERMS_INTO(m, m->workspace, {
      mtp->dstats = dstats; /* Stuck the change statistic here.*/
      if(mtp->c_func==NULL && mtp->d_func)
	(*(mtp->d_func))(ntoggles, tails, heads, weights,
			 mtp, nwp);  /* Call d_??? function */
    });
  /* Notice that mtp->dstats now points to the appropriate location in
     m->workspace. */
  
  /* Put the original destination dstats back unless there's only one
     toggle. */
  if(ntoggles!=1){
    unsigned int i = 0;
    WtEXEC_THROUGH_TERMS(m, {
	mtp->dstats = m->dstatarray[i];
	i++;
      });
  }

  /* Make a pass through terms with c_functions. */
  FOR_EACH_TOGGLE{
    GETTOGGLEINFO();
    
    ergm_PARALLEL_FOR_LIMIT(m->n_terms)
    WtEXEC_THROUGH_TERMS_INTO(m, m->workspace, {
	if(mtp->c_func){
	  if(ntoggles!=1) ZERO_ALL_CHANGESTATS();
	  (*(mtp->c_func))(TAIL, HEAD, NEWWT,
			   mtp, nwp, OLDWT);  /* Call d_??? function */
	  if(ntoggles!=1){
            addonto(dstats, mtp->dstats, N_CHANGE_STATS);
	  }
	}
      });

    /* Update storage and network */    
    IF_MORE_TO_COME{
      SETWT_WITH_BACKUP();
    }
  }
}


void WtChangeStatsUndo(unsigned int ntoggles, Vertex *tails, Vertex *heads, double *weights,
                       WtNetwork *nwp, WtModel *m){
  UNDO_PREVIOUS{
    GETOLDTOGGLEINFO();
    SETWT(TAIL,HEAD,weights[TOGGLEIND]);
    weights[TOGGLEIND]=OLDWT;
  }
}


/*
  WtChangeStats
  A helper's helper function to compute change statistics.
  The vector of changes is written to m->workspace.
*/
void WtChangeStats(unsigned int ntoggles, Vertex *tails, Vertex *heads, double *weights,
				 WtNetwork *nwp, WtModel *m){
  WtChangeStatsDo(ntoggles, tails, heads, weights, nwp, m);
  WtChangeStatsUndo(ntoggles, tails, heads, weights, nwp, m);
}

/*
  WtChangeStats1
  A simplified version of WtChangeStats for exactly one change.
*/
void WtChangeStats1(Vertex tail, Vertex head, double weight,
                    WtNetwork *nwp, WtModel *m, double edgestate){
  memset(m->workspace, 0, m->n_stats*sizeof(double)); /* Zero all change stats. */

  /* Make a pass through terms with c_functions. */
  ergm_PARALLEL_FOR_LIMIT(m->n_terms)
    WtEXEC_THROUGH_TERMS_INTO(m, m->workspace, {
        mtp->dstats = dstats; /* Stuck the change statistic here.*/
        if(mtp->c_func){
          (*(mtp->c_func))(tail, head, weight,
                           mtp, nwp, edgestate);  /* Call c_??? function */
        }else if(mtp->d_func){
          (*(mtp->d_func))(1, &tail, &head, &weight,
                           mtp, nwp);  /* Call d_??? function */
        }
      });
}


/*
  WtZStats
  Call baseline statistics calculation (for extended state).
*/
void WtZStats(WtNetwork *nwp, WtModel *m, Rboolean skip_s){
  memset(m->workspace, 0, m->n_stats*sizeof(double)); /* Zero all change stats. */

  /* Make a pass through terms with c_functions. */
  ergm_PARALLEL_FOR_LIMIT(m->n_terms)
    WtEXEC_THROUGH_TERMS_INTO(m, m->workspace, {
        mtp->dstats = dstats; /* Stuck the change statistic here.*/
        if(!skip_s || mtp->s_func==NULL)
          if(mtp->z_func)
            (*(mtp->z_func))(mtp, nwp, skip_s);  /* Call z_??? function */
      });
}

/*
  WtEmptyNetworkStats
  Extract constant empty network stats.
*/
void WtEmptyNetworkStats(WtModel *m, Rboolean skip_s){
  memset(m->workspace, 0, m->n_stats*sizeof(double)); /* Zero all change stats. */

  WtEXEC_THROUGH_TERMS_INTO(m, m->workspace, {
      if(!skip_s || mtp->s_func==NULL){
        if(mtp->emptynwstats)
          memcpy(dstats, mtp->emptynwstats, mtp->nstats*sizeof(double));
      }});
}

/****************
 void WtSummStats

Compute summary statistics. It has two modes:
* nwp is empty and m is initialized consistently with nwp -> use edgelist
* nwp is not empty n_edges=0, and m does not have to be initialized consistently with nwp -> use nwp (making temporary copies of nwp and reinitializing m)
*****************/
void WtSummStats(Edge n_edges, Vertex *tails, Vertex *heads, double *weights, WtNetwork *nwp, WtModel *m){
  Rboolean mynet;
  double *stats;
  if(EDGECOUNT(nwp)){
    if(n_edges) error("WtSummStats must be passed either an empty network and a list of edges or a non-empty network and no edges.");
    /* The following code is pretty inefficient, but it'll do for now. */
    /* Grab network state and output workspace. */
    n_edges = EDGECOUNT(nwp);
    /* Use R's memory management to make the routine interruptible.

       TODO: Check how much overhead this incurs over and above
       in-house on.exit() memory management.
    */
    tails = (Vertex *) INTEGER(PROTECT(allocVector(INTSXP, n_edges)));
    heads = (Vertex *) INTEGER(PROTECT(allocVector(INTSXP, n_edges)));
    weights = REAL(PROTECT(allocVector(REALSXP, n_edges)));

    WtEdgeTree2EdgeList(tails, heads, weights, nwp, n_edges);
    stats = m->workspace;

    /* Replace the model and network with an empty one. */
    nwp = WtNetworkInitialize(NULL, NULL, NULL, n_edges, N_NODES, DIRECTED, BIPARTITE, 0, 0, NULL);
    m = WtModelInitialize(m->R, m->ext_state, nwp, TRUE);
    mynet = TRUE;
  }else{
    /* Use R's memory management to make the routine interruptible.

       TODO: Check how much overhead this incurs over and above
       in-house on.exit() memory management.
    */
    stats = REAL(PROTECT(allocVector(REALSXP, m->n_stats)));
    mynet = FALSE;
  }

  memset(stats, 0, m->n_stats*sizeof(double));

  WtEmptyNetworkStats(m, TRUE);
  addonto(stats, m->workspace, m->n_stats);
  WtZStats(nwp, m, TRUE);
  addonto(stats, m->workspace, m->n_stats);

  WtDetShuffleEdges(tails,heads,weights,n_edges); /* Shuffle edgelist. */

  Edge ntoggles = n_edges; // So that we can use the macros

  /* Calculate statistics for terms that don't have c_functions or s_functions.  */
  WtEXEC_THROUGH_TERMS_INTO(m, stats, {
      if(mtp->s_func==NULL && mtp->c_func==NULL && mtp->d_func){
	(*(mtp->d_func))(ntoggles, tails, heads, weights,
			 mtp, nwp);  /* Call d_??? function */
	addonto(dstats, mtp->dstats, N_CHANGE_STATS);
      }
    });

  /* Calculate statistics for terms that have c_functions but not s_functions.  */
  FOR_EACH_TOGGLE{
    GETNEWTOGGLEINFO();

    ergm_PARALLEL_FOR_LIMIT(m->n_terms)
    WtEXEC_THROUGH_TERMS_INTO(m, stats, {
	if(mtp->s_func==NULL && mtp->c_func){
	  ZERO_ALL_CHANGESTATS();
	  (*(mtp->c_func))(TAIL, HEAD, NEWWT,
			   mtp, nwp, 0);  /* Call c_??? function */
	    addonto(dstats, mtp->dstats, N_CHANGE_STATS);
	}
      });

    /* Update storage and network */
    SETWT(TAIL, HEAD, NEWWT);
  }

  /* Calculate statistics for terms have s_functions  */
  WtEXEC_THROUGH_TERMS_INTO(m, stats, {
      if(mtp->s_func){
	ZERO_ALL_CHANGESTATS();
	(*(mtp->s_func))(mtp, nwp);  /* Call d_??? function */
	for(unsigned int k=0; k<N_CHANGE_STATS; k++){
	  dstats[k] = mtp->dstats[k]; // Overwrite, not accumulate.
	}
      }
    });

  if(mynet){
    WtModelDestroy(nwp,m);
    WtNetworkDestroy(nwp);
    UNPROTECT(3);
  }else{
    WtDetUnShuffleEdges(tails,heads,weights,n_edges); /* Unshuffle edgelist. */
    memcpy(m->workspace, stats, m->n_stats*sizeof(double));
    UNPROTECT(1);
  }
}
