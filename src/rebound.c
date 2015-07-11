/**
 * @file 	main.c
 * @brief 	Main routine, iteration loop, timing.
 * @author 	Hanno Rein <hanno@hanno-rein.de>
 * 
 * @section 	LICENSE
 * Copyright (c) 2011 Hanno Rein, Shangfei Liu
 *
 * This file is part of rebound.
 *
 * rebound is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * rebound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rebound.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include "integrator.h"
#include "integrator_whfast.h"
#include "boundaries.h"
#include "gravity.h"
#include "output.h"
#include "collisions.h"
#include "tree.h"
#include "tools.h"
#include "particle.h"
#include "rebound.h"
#include "communication_mpi.h"
#ifdef OPENGL
#include "display.h"
#endif // OPENGL
#ifdef OPENMP
#include <omp.h>
#endif
#ifdef GRAVITY_GRAPE
void gravity_finish(void);
#endif // GRAVITY_GRAPE

static const char* logo[];				/**< Logo of rebound. */
static const char* build_str = __DATE__ " " __TIME__;	/**< Date and time build string. */


// Global (non-thread safe) variables:
unsigned int rebound_show_logo = 1;


void rebound_step(struct Rebound* const r){
	// A 'DKD'-like integrator will do the first 'D' part.
	PROFILING_START()
	integrator_part1(r);
	PROFILING_STOP(PROFILING_CAT_INTEGRATOR)

	// Check for root crossings.
	PROFILING_START()
	boundaries_check();     
	PROFILING_STOP(PROFILING_CAT_BOUNDARY)

	// Update and simplify tree. 
	// Prepare particles for distribution to other nodes. 
	// This function also creates the tree if called for the first time.
	PROFILING_START()
#ifdef TREE
	tree_update();          
#endif //TREE

#ifdef MPI
	// Distribute particles and add newly received particles to tree.
	communication_mpi_distribute_particles();
#endif // MPI

#ifdef GRAVITY_TREE
	// Update center of mass and quadrupole moments in tree in preparation of force calculation.
	tree_update_gravity_data(); 
	
#ifdef MPI
	// Prepare essential tree (and particles close to the boundary needed for collisions) for distribution to other nodes.
	tree_prepare_essential_tree_for_gravity();

	// Transfer essential tree and particles needed for collisions.
	communication_mpi_distribute_essential_tree_for_gravity();
#endif // MPI
#endif // GRAVITY_TREE

	// Calculate accelerations. 
	gravity_calculate_acceleration(r);
	if (r->N_megno){
		gravity_calculate_variational_acceleration(r);
	}
	// Calculate non-gravity accelerations. 
	if (r->additional_forces) r->additional_forces(r);
	PROFILING_STOP(PROFILING_CAT_GRAVITY)

	// A 'DKD'-like integrator will do the 'KD' part.
	PROFILING_START()
	integrator_part2(r);
	if (r->post_timestep_modifications){
		integrator_synchronize(r);
		r->post_timestep_modifications(r);
		r->ri_whfast.recalculate_jacobi_this_timestep = 1;
	}
	PROFILING_STOP(PROFILING_CAT_INTEGRATOR)

	// Do collisions here. We need both the positions and velocities at the same time.
#ifndef COLLISIONS_NONE
	// Check for root crossings.
	PROFILING_START()
	boundaries_check();     
	PROFILING_STOP(PROFILING_CAT_BOUNDARY)

	// Search for collisions using local and essential tree.
	PROFILING_START()
	collisions_search();

	// Resolve collisions (only local particles are affected).
	collisions_resolve();
	PROFILING_STOP(PROFILING_CAT_COLLISION)
#endif  // COLLISIONS_NONE
}

struct Rebound* rebound_init(){
	if (rebound_show_logo==1){
		int i =0;
		while (logo[i]!=NULL){ printf("%s",logo[i++]); }
		printf("Built: %s\n\n",build_str);
	}
	tools_init_srand();
	struct Rebound* r = calloc(1,sizeof(struct Rebound));
	r->t 		= 0; 
	r->G 		= 1;
	r->softening 	= 0;
	r->dt		= 0.001;
	r->boxsize 	= -1;
	r->boxsize_x 	= -1;
	r->boxsize_y 	= -1;
	r->boxsize_z 	= -1;
	r->boxsize_max 	= -1;
	r->root_nx	= 1;
	r->root_ny	= 1;
	r->root_nz	= 1;
	r->root_n	= 1;
	r->N 		= 0;	
	r->Nmax		= 0;	
	r->N_active 	= -1; 	
	r->N_megno 	= 0; 	
	r->exit_simulation	= 0;
	r->exact_finish_time 	= 0;
	r->particles	= NULL;
	r->integrator = IAS15;
	r->force_is_velocitydependent = 0;

	// Function pointers 
	r->additional_forces 		= NULL;
	r->finished			= NULL;
	r->post_timestep		= NULL;
	r->post_timestep_modifications	= NULL;

	// Integrators	
	// ********** WHFAST
	// the defaults below are chosen to safeguard the user against spurious results, but
	// will be slower and less accurate
	r->ri_whfast.corrector = 0;
	r->ri_whfast.safe_mode = 1;
	r->ri_whfast.recalculate_jacobi_this_timestep = 0;
	r->ri_whfast.is_synchronized = 1;
	r->ri_whfast.allocated_N = 0;
	r->ri_whfast.timestep_warning = 0;
	r->ri_whfast.recalculate_jacobi_but_not_synchronized_warning = 0;
	
	// ********** IAS15
	r->ri_ias15.epsilon 		= 1e-9;
	r->ri_ias15.min_dt 		= 0;
	r->ri_ias15.epsilon_global	= 1;
	r->ri_ias15.iterations_max_exceeded= 0;	

	if (r->root_nx <=0 || r->root_ny <=0 || r->root_nz <= 0){
		fprintf(stderr,"ERROR: Number of root boxes must be greater or equal to 1 in each direction.\n");
		exit(-1);
	}

	// Setup box sizes
	//boxsize_x = boxsize *(double)root_nx;
	//boxsize_y = boxsize *(double)root_ny;
	//boxsize_z = boxsize *(double)root_nz;
	//root_n = root_nx*root_ny*root_nz;

	// Make sure domain can be decomposed into equal number of root boxes per node.
	//if ((root_n/mpi_num)*mpi_num != root_n){
	//	if (mpi_id==0) fprintf(stderr,"ERROR: Number of root boxes (%d) not a multiple of mpi nodes (%d).\n",root_n,mpi_num);
	//	exit(-1);
	//}

	//boxsize_max = boxsize_x;
	//if (boxsize_max<boxsize_y) boxsize_max = boxsize_y;
	//if (boxsize_max<boxsize_z) boxsize_max = boxsize_z;
	
#ifdef MPI
	printf("Initialized %d*%d*%d root boxes. MPI-node: %d. Process id: %d.\n",r->root_nx,r->root_ny,r->root_nz,mpi_id, getpid());
#else // MPI
	printf("Initialized %d*%d*%d root boxes. Process id: %d.\n",r->root_nx,r->root_ny,r->root_nz, getpid());
#endif // MPI
#ifdef OPENMP
	printf("Using OpenMP with %d threads per node.\n",omp_get_max_threads());
#endif // OPENMP
	return r;
}

int rebound_integrate(struct Rebound* const r, double tmax, double maxR, double minD){
	struct timeval tim;
	gettimeofday(&tim, NULL);
	double timing_initial = tim.tv_sec+(tim.tv_usec/1000000.0);
	double dt_last_done = r->dt;
	int last_step = 0;
	int ret_value = 0;
	const double dtsign = copysign(1.,r->dt); 				// Used to determine integration direction
	if (r->post_timestep){ r->post_timestep(r); }
	if ((r->t+r->dt)*dtsign>=tmax*dtsign && r->exact_finish_time==1){
		r->dt = tmax-r->t;
		last_step++;
	}
	while(r->t*dtsign<tmax*dtsign && last_step<2 && ret_value==0 && r->exit_simulation==0){
		if (r->N<=0){
			fprintf(stderr,"\n\033[1mError!\033[0m No particles found. Exiting.\n");
			return(1);
		}
		rebound_step(r); 								// 0 to not do timing within step
#ifdef OPENGL
		PROFILING_START()
		display();
		PROFILING_STOP(PROFILING_CAT_VISUALIZATION)
#endif // OPENGL
		if ((r->t+r->dt)*dtsign>=tmax*dtsign && r->exact_finish_time==1){
			integrator_synchronize(r);
			r->dt = tmax-r->t;
			last_step++;
		}else{
			dt_last_done = r->dt;
		}
		if (r->post_timestep){ r->post_timestep(r); }
		if (maxR){
			// Check for escaping particles
			const double maxR2 = maxR*maxR;
			const int N = r->N - r->N_megno;
			const struct Particle* const particles = r->particles;
			for (int i=0;i<N;i++){
				struct Particle p = particles[i];
				double r2 = p.x*p.x + p.y*p.y + p.z*p.z;
				if (r2>maxR2){
					ret_value = 2;
#warning TODO
					//escapedParticle = i;
				}
			}
		}
		if (minD){
			// Check for close encounters
			const double minD2 = minD*minD;
			const int N = r->N - r->N_megno;
			const struct Particle* const particles = r->particles;
			for (int i=0;i<N;i++){
				struct Particle pi = particles[i];
				for (int j=0;j<i;j++){
					struct Particle pj = particles[j];
					const double x = pi.x-pj.x;
					const double y = pi.y-pj.y;
					const double z = pi.z-pj.z;
					const double r2 = x*x + y*y + z*z;
					if (r2<minD2){
#warning TODO
						ret_value = 3;
					 	//closeEncounterPi = i;
						//closeEncounterPj = j;
					}
				}
			}
		}
	}
	integrator_synchronize(r);
	r->dt = dt_last_done;
	gettimeofday(&tim, NULL);
	double timing_final = tim.tv_sec+(tim.tv_usec/1000000.0);
	printf("\nComputation finished. Total runtime: %f s\n",timing_final-timing_initial);
	return ret_value;
}

static const char* logo[] = {
"          _                           _  \n",   
"         | |                         | | \n",  
" _ __ ___| |__   ___  _   _ _ __   __| | \n", 
"| '__/ _ \\ '_ \\ / _ \\| | | | '_ \\ / _` | \n", 
"| | |  __/ |_) | (_) | |_| | | | | (_| | \n", 
"|_|  \\___|_.__/ \\___/ \\__,_|_| |_|\\__,_| \n", 
"                                         \n",   
"              `-:://::.`                 \n",
"          `/oshhoo+++oossso+:`           \n", 
"       `/ssooys++++++ossssssyyo:`        \n", 
"     `+do++oho+++osssso++++++++sy/`      \n", 
"    :yoh+++ho++oys+++++++++++++++ss.     \n", 
"   /y++hooyyooshooo+++++++++++++++oh-    \n", 
"  -dsssdssdsssdssssssssssooo+++++++oh`   \n", 
"  ho++ys+oy+++ho++++++++oosssssooo++so   \n", 
" .d++oy++ys+++oh+++++++++++++++oosssod   \n", 
" -h+oh+++yo++++oyo+++++++++++++++++oom   \n", 
" `d+ho+++ys+++++oys++++++++++++++++++d   \n", 
"  yys++++oy+++++++oys+++++++++++++++s+   \n", 
"  .m++++++h+++++++++oys++++++++++++oy`   \n", 
"   -yo++++ss++++++++++oyso++++++++oy.    \n", 
"    .ss++++ho+++++++++++osys+++++yo`     \n", 
"      :ss+++ho+++++++++++++osssss-       \n", 
"        -ossoys++++++++++++osso.         \n", 
"          `-/oyyyssosssyso+/.            \n", 
"                ``....`                  \n", 
"                                         \n",   
"Written by Hanno Rein, Shangfei Liu,     \n",
"David Spiegel, Daniel Tamayo and many    \n",
"other. REBOUND project website:          \n",  
"http://github.com/hannorein/rebound/     \n",    
"                                         \n", 
NULL};