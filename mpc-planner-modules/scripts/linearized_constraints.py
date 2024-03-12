"""
Linearized constraints (w.r.t to the vector from robot to obstacle)
"""

import numpy as np

import sys
import os

sys.path.append(os.path.join(sys.path[0], "..", "..", "mpc-planner-solver-generator"))


from util.math import rotation_matrix
from control_modules import ConstraintModule


class LinearizedConstraintModule(ConstraintModule):

    def __init__(self, settings):
        super().__init__()

        self.module_name = "LinearizedConstraints"  # c++ name of the module
        self.import_name = "linearized_constraints.h"

        self.constraints.append(LinearConstraints(n_discs=settings["n_discs"], n_constraints=settings["max_obstacles"]))
        self.description = "Linearized dynamic collision avoidance constraints"


# Constraints of the form Ax <= b (+ slack)
class LinearConstraints:

    def __init__(self, n_discs, max_obstacles):
        self.n_discs = n_discs
        self.max_obstacles = max_obstacles
        self.n_constraints = max_obstacles * n_discs
        self.nh = self.n_constraints

    def define_parameters(self, params):

        for disc_id in range(self.n_discs):
            params.add(f"ego_disc_{disc_id}_offset")

            for index in range(self.max_obstacles):
                params.add(self.constraint_name(index, disc_id) + "_a1")
                params.add(self.constraint_name(index, disc_id) + "_a2")
                params.add(self.constraint_name(index, disc_id) + "_b")

    def constraint_name(self, index, disc_id):
        return f"disc_{disc_id}_lin_constraint_{index}"

    def get_lower_bound(self):
        lower_bound = []
        for index in range(0, self.n_constraints):
            lower_bound.append(-np.inf)
        return lower_bound

    def get_upper_bound(self):
        upper_bound = []
        for index in range(0, self.n_constraints):
            upper_bound.append(0.0)
        return upper_bound

    def get_constraints(self, model, params, settings, stage_idx):
        constraints = []

        # States
        pos_x = model.get("x")
        pos_y = model.get("y")
        pos = np.array([pos_x, pos_y])
        psi = model.get("psi")

        rotation_car = rotation_matrix(psi)
        for disc_it in range(self.n_discs):
            disc_x = params.get(f"ego_disc_{disc_it}_offset")
            disc_relative_pos = np.array([disc_x, 0])
            disc_pos = pos + rotation_car.dot(disc_relative_pos)

            for index in range(self.max_obstacles):
                a1 = params.get(self.constraint_name(index, disc_it) + "_a1")
                a2 = params.get(self.constraint_name(index, disc_it) + "_a2")
                b = params.get(self.constraint_name(index, disc_it) + "_b")

                constraints.append(a1 * disc_pos[0] + a2 * disc_pos[1] - b)

        return constraints
