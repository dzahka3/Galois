/*
 * main.cpp
 *
 *  Created on: Nov 11, 2010
 *      Author: amshali
 */

#include "Barneshut.h"
#include "Galois/Launcher.h"
#include "Galois/Runtime/Timer.h"
#include <iostream>
#include <math.h>
#include <algorithm>
#include <sys/time.h>

#include "Lonestar/Banner.h"
#include "Lonestar/CommandLine.h"

static const char* name = "Barnshut N-Body Simulator";
static const char* description = "Simulation of the gravitational forces in a galactic cluster using the Barnes-Hut n-body algorithm\n";
static const char* url = "http://iss.ices.utexas.edu/lonestar/barneshut.html";
static const char* help = "[file <input file>|gen <numbodies> <ntimesteps> <seed>]";

Graph *octree;
GNode root;
int step;
Barneshut barneshut;

struct process {
  template<typename Context>
  void operator()(GNode& item, Context& lwl) {
    barneshut.computeForce(item, octree, root, barneshut.diameter,
			   barneshut.itolsq, step, barneshut.dthf, barneshut.epssq);
  }
};

void readInput(std::vector<const char*>& args, Barneshut& app) {
  if (args.size() < 2) {
    std::cerr << "not enough arguments, use -help for usage information\n";
    exit(1);
  } else if (strcmp(args[0], "file") == 0 && args.size() > 1) {
    app.readInput(args[1]);
  } else if (strcmp(args[0], "gen") == 0 && args.size() > 3) {
    app.genInput(atoi(args[1]), atoi(args[2]), atoi(args[3]));
  } else {
    std::cerr << "wrong arguments, use -help for usage information\n";
    exit(1);
  }
}

void pmain(int, int, int);

int main(int argc, const char** argv) {
  std::cout.setf(std::ios::right|std::ios::scientific|std::ios::showpoint);

  std::vector<const char*> args = parse_command_line(argc, argv, help);
  readInput(args, barneshut);
  printBanner(std::cout, name, description, url);
  std::cerr << "configuration: "
            << barneshut.nbodies << " bodies, "
            << barneshut.ntimesteps << " time steps" << std::endl << std::endl;
	std::cout << "Num. of threads: " << numThreads << std::endl;

  if (true) {
    Galois::Launcher::startTiming();
    pmain(barneshut.nbodies, barneshut.ntimesteps, barneshut.seed);
    Galois::Launcher::stopTiming();
    std::cout << "STAT: Time " << Galois::Launcher::elapsedTime() << "\n";
  }

	Galois::Launcher::startTiming();
	OctTreeNodeData res;
	for (step = 0; step < barneshut.ntimesteps; step++) {
		barneshut.computeCenterAndDiameter();

    octree = new Graph;
		root = createNode(octree, OctTreeNodeData(barneshut.centerx,
				barneshut.centery, barneshut.centerz)); 
    octree->addNode(root);

    barneshut.insertPoints(octree, root);

    // summarize subtree info in each internal node
    // (plus restructure tree and sort bodies for performance reasons)
    barneshut.curr = 0;
		barneshut.computeCenterOfMass(octree, root);

		GaloisRuntime::WorkList::ChunkedBag<GNode, 256> wl;
		wl.fill_initial(&barneshut.leaf[0], &barneshut.leaf[barneshut.curr]);
		Galois::for_each(wl, process());

    // advance the position and velocity of each
		barneshut.advance(octree, barneshut.dthf, barneshut.dtime);

		if (Galois::Launcher::isFirstRun()) {
			res = root.getData(Galois::Graph::NONE);
			std::cout << "Timestep " << step << " Center of Mass = " << res.posx
					<< " " << res.posy << " " << res.posz << std::endl;
		}
		delete octree;
	} 
	Galois::Launcher::stopTiming();
	std::cout << "STAT: Time " << Galois::Launcher::elapsedTime() << "\n";

	if (Galois::Launcher::isFirstRun() && !skipVerify) { // verify result
		Barneshut b2;
    readInput(args, b2);
		OctTreeNodeData s_res;
		for (step = 0; step < b2.ntimesteps; step++) {
			b2.computeCenterAndDiameter();
			Graph *octree2 = new Graph;
			root = createNode(octree2, OctTreeNodeData(b2.centerx, b2.centery, b2.centerz));
			octree2->addNode(root);
      b2.insertPoints(octree2, root);
			b2.curr = 0;
			b2.computeCenterOfMass(octree2, root);

			for (int kk = 0; kk < b2.curr; kk++) {
				b2.computeForce(b2.leaf[kk], octree2, root,
						b2.diameter, b2.itolsq, step, b2.dthf,
						b2.epssq);
			}
			b2.advance(octree2, b2.dthf, b2.dtime);
			s_res = root.getData(Galois::Graph::NONE);
			delete octree2;
		}

		if ((fabs(res.posx - s_res.posx) / fabs(std::min(res.posx, s_res.posx))
				> 0.001) || (fabs(res.posy - s_res.posy) / fabs(std::min(res.posy,
				s_res.posy)) > 0.001) || (fabs(res.posz - s_res.posz) / fabs(std::min(
				res.posz, s_res.posz)) > 0.001)) {
			std::cerr << "verification failed" << std::endl;
		} else {
			std::cerr << "verification succeeded" << std::endl;
		}
	}
}
