#include "mpc-planner-solver/solver_interface.h"

extern "C"
{
	Solver_extfunc extfunc_eval_ = &Solver_adtool2forces;
}

namespace MPCPlanner
{

	State::State()
	{
		loadConfigYaml(SYSTEM_CONFIG_PATH(__FILE__, "solver_settings"), _config);
		loadConfigYaml(SYSTEM_CONFIG_PATH(__FILE__, "model_map"), _model_map);
		initialize();
	}

	Solver::Solver(int solver_id)
	{
		_solver_id = solver_id;
		_solver_memory = (char *)malloc(Solver_get_mem_size());
		_solver_memory_handle = Solver_external_mem(_solver_memory, _solver_id, Solver_get_mem_size());
		loadConfigYaml(SYSTEM_CONFIG_PATH(__FILE__, "solver_settings"), _config);
		loadConfigYaml(SYSTEM_CONFIG_PATH(__FILE__, "parameter_map"), _parameter_map);
		loadConfigYaml(SYSTEM_CONFIG_PATH(__FILE__, "model_map"), _model_map);
		N = _config["N"].as<unsigned int>();
		nu = _config["nu"].as<unsigned int>();
		nx = _config["nx"].as<unsigned int>();
		nvar = _config["nvar"].as<unsigned int>();
		npar = _config["npar"].as<unsigned int>();
		// LOG_INFO(nx << ", " << nu << ", " << N);
		reset();
	}

	void Solver::reset()
	{
		for (long int i = 0; i < *(&_params.all_parameters + 1) - _params.all_parameters; i++)
			_params.all_parameters[i] = 0.0;

		for (long int i = 0; i < *(&_params.xinit + 1) - _params.xinit; i++)
			_params.xinit[i] = 0.0;

		for (size_t i = 0; i < N * nvar; i++)
			_params.x0[i] = 0.0;
	}

	Solver::~Solver()
	{
		free(_solver_memory);
	}

	void Solver::setParameter(int k, std::string &&parameter, double value)
	{
		_params.all_parameters[k * npar + _parameter_map[parameter].as<int>()] = value;
	}

	double Solver::getParameter(int k, std::string &&parameter)
	{
		return _params.all_parameters[k * npar + _parameter_map[parameter].as<int>()];
	}

	void Solver::setXinit(std::string &&state_name, double value)
	{
		_params.xinit[_model_map[state_name][1].as<int>() - nu] = value;
	}

	void Solver::setVar(unsigned int k, std::string &&var_name, double value)
	{
		int index = _model_map[var_name][1].as<int>();
		_params.x0[k * nvar + index] = value;
	}

	double Solver::getVar(unsigned int k, std::string &&var_name)
	{
		int index = _model_map[var_name][1].as<int>();
		return _params.x0[k * nvar + index];
	}

	int Solver::solve()
	{
		int exit_code = Solver_solve(&_params, &_output, &_info, _solver_memory_handle, stdout, extfunc_eval_);
		return exit_code;
	}

	double Solver::getOutput(int k, std::string &&state_name)
	{
		if (k == 0)
			return _output.x1[_model_map[state_name][1].as<int>()];
		if (k == 1)
			return _output.x2[_model_map[state_name][1].as<int>()];
	}
};