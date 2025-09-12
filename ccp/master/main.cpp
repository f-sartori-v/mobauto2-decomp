// CP Optimizer hello world (C++)
// Build example (macOS, CPLEX Studio 22.1.1):
//   clang++ -O2 -std=c++17 \
//     -I/Applications/CPLEX_Studio2211/concert/include \
//     -I/Applications/CPLEX_Studio2211/cplex/include \
//     -I/Applications/CPLEX_Studio2211/cpoptimizer/include \
//     ccp/master/main.cpp \
//     -L/Applications/CPLEX_Studio2211/concert/lib/x86-64_osx/static_pic \
//     -L/Applications/CPLEX_Studio2211/cplex/lib/x86-64_osx/static_pic \
//     -L/Applications/CPLEX_Studio2211/cpoptimizer/lib/x86-64_osx/static_pic \
//     -lconcert -lilocplex -lcplex -lcp -lpthread -lm -o ccp/master.out

#include <ilcp/cp.h>
#include <iostream>

int main(int argc, char** argv) {
    IloEnv env;
    int exit_code = 0;
    try {
        IloModel model(env);

        // Simple CP model: choose x in [0, 10], minimize x, with x >= 1
        IloIntVar x(env, 0, 10, "x");
        model.add(x >= 1);
        model.add(IloMaximize(env, x));

        IloCP cp(model);
        cp.setParameter(IloCP::LogVerbosity, IloCP::Quiet);
        bool ok = cp.solve();
        if (ok) {
            std::cout << "[cp] status=Optimal value x=" << cp.getValue(x) << std::endl;
        } else {
            std::cout << "[cp] no solution found" << std::endl;
            exit_code = 1;
        }
    } catch (const IloException& e) {
        std::cerr << "Concert exception: " << e << std::endl;
        exit_code = 1;
    } catch (const std::exception& e) {
        std::cerr << "std::exception: " << e.what() << std::endl;
        exit_code = 1;
    } catch (...) {
        std::cerr << "Unknown exception" << std::endl;
        exit_code = 1;
    }
    env.end();
    return exit_code;
}

